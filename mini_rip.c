#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define PORT 5555
#define MAX_ROUTES 32
#define MAX_IFACES 16
#define INFINITY_COST 16
#define UPDATE_INTERVAL 1
#define DEAD_INTERVAL 3

typedef struct {
    char ifName[16];
    char ipAddr[20];
    char subnetPrefix[20]; // Ex: "10.0.1.0/24"
    char broadcastIp[20];  // Ex: "10.0.1.255"
    uint32_t netAddr;      // Rede em binario para comparacao de Split Horizon
    uint32_t netMask;
} LocalInterface;

typedef struct {
    char subnet[20];
    int metric;
} RouteAdvertisement;

typedef struct {
    char senderName[16];
    int routeCount;
    RouteAdvertisement routes[MAX_ROUTES];
} RipPacket;

typedef struct {
    char subnet[20];
    int metric;
    char nextHop[20];
    time_t lastUpdated;
    int directlyConnected;
} RouteEntry;

char hostName[16];
LocalInterface localIfaces[MAX_IFACES];
int ifaceCount = 0;

RouteEntry routingTable[MAX_ROUTES];
int routeCount = 0;

// Injeta ou remove a rota direto na tabela do Linux
void applyKernelRoute(const char *subnet, const char *nextHop, int deleteRoute) {
    char cmd[128];
    if (deleteRoute) {
        snprintf(cmd, sizeof(cmd), "ip route del %s 2>/dev/null", subnet);
    } else {
        snprintf(cmd, sizeof(cmd), "ip route replace %s via %s 2>/dev/null", subnet, nextHop);
    }
    system(cmd);
}

// Verifica se o IP que enviou o pacote eh do proprio roteador
int isMyOwnIp(const char *ip) {
    for (int i = 0; i < ifaceCount; i++) {
        if (strcmp(localIfaces[i].ipAddr, ip) == 0) {
            return 1;
        }
    }
    return 0;
}

// Verifica se um IP pertence aa sub-rede de uma determinada interface
int isIpInInterfaceSubnet(const char *ipStr, LocalInterface *iface) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ipStr, &addr) <= 0) return 0;
    return (addr.s_addr & iface->netMask) == iface->netAddr;
}

// Busca a rota na tabela ou cria uma nova entrada vazia
int findOrCreateRoute(const char *subnet) {
    for (int i = 0; i < routeCount; i++) {
        if (strcmp(routingTable[i].subnet, subnet) == 0) {
            return i;
        }
    }
    if (routeCount >= MAX_ROUTES) return -1;

    int idx = routeCount++;
    strncpy(routingTable[idx].subnet, subnet, 20);
    routingTable[idx].metric = INFINITY_COST;
    strcpy(routingTable[idx].nextHop, "0.0.0.0");
    routingTable[idx].lastUpdated = 0;
    routingTable[idx].directlyConnected = 0;
    return idx;
}

// Descobre todas as interfaces ativas do container e adiciona as sub-redes como rotas locais
void discoverLocalInterfaces() {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(1);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue; // Ignora localhost
        if (!(ifa->ifa_flags & IFF_UP)) continue;    // Ignora interfaces caidas

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;

        uint32_t ipBin = addr->sin_addr.s_addr;
        uint32_t maskBin = mask->sin_addr.s_addr;
        uint32_t netBin = ipBin & maskBin;
        uint32_t bcastBin = netBin | (~maskBin);

        struct in_addr netInAddr = { .s_addr = netBin };
        struct in_addr bcastInAddr = { .s_addr = bcastBin };

        char ipStr[20], netStr[20], bcastStr[20];
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
        inet_ntop(AF_INET, &netInAddr, netStr, sizeof(netStr));
        inet_ntop(AF_INET, &bcastInAddr, bcastStr, sizeof(bcastStr));

        // Filtra apenas as sub-redes de teste da topologia (10.0.x.0/24)
        if (strncmp(ipStr, "10.0.", 5) != 0) continue;

        LocalInterface *lif = &localIfaces[ifaceCount++];
        strncpy(lif->ifName, ifa->ifa_name, 16);
        strncpy(lif->ipAddr, ipStr, 20);
        snprintf(lif->subnetPrefix, 20, "%s/24", netStr);
        strncpy(lif->broadcastIp, bcastStr, 20);
        lif->netAddr = netBin;
        lif->netMask = maskBin;

        // Cadastra como rota diretamente conectada
        int rIdx = findOrCreateRoute(lif->subnetPrefix);
        if (rIdx >= 0) {
            routingTable[rIdx].metric = 0;
            strcpy(routingTable[rIdx].nextHop, "0.0.0.0");
            routingTable[rIdx].directlyConnected = 1;
            routingTable[rIdx].lastUpdated = time(NULL);
        }

        printf("[%s] Interface detectada: %s | IP: %s | Rede: %s | Broadcast: %s\n",
               hostName, lif->ifName, lif->ipAddr, lif->subnetPrefix, lif->broadcastIp);
    }

    freeifaddrs(ifaddr);
}

// Dispara a tabela de roteamento inteira via UDP Broadcast
void sendRoutingUpdates(int sockfd) {
    struct sockaddr_in destAddr;
    memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(PORT);

    // Envia um anuncio por interface aplicando Split Horizon com Poison Reverse
    for (int i = 0; i < ifaceCount; i++) {
        RipPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        strncpy(pkt.senderName, hostName, 16);
        pkt.routeCount = routeCount;

        for (int r = 0; r < routeCount; r++) {
            strncpy(pkt.routes[r].subnet, routingTable[r].subnet, 20);

            // Regra Anti-Loop (Poison Reverse): Se aprendi a rota por essa interface, anuncio com custo infinito nela
            if (!routingTable[r].directlyConnected &&
                isIpInInterfaceSubnet(routingTable[r].nextHop, &localIfaces[i])) {
                pkt.routes[r].metric = INFINITY_COST; 
            } else {
                pkt.routes[r].metric = routingTable[r].metric;
            }
        }

        inet_pton(AF_INET, localIfaces[i].broadcastIp, &destAddr.sin_addr);
        sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
    }
}

// Analisa os pacotes recebidos dos vizinhos e recalcula as rotas
int processIncomingPacket(RipPacket *pkt, const char *senderIp, time_t now) {
    int changed = 0;

    for (int i = 0; i < pkt->routeCount; i++) {
        int rIdx = findOrCreateRoute(pkt->routes[i].subnet);
        if (rIdx < 0 || routingTable[rIdx].directlyConnected) continue;

        int newCost = pkt->routes[i].metric + 1; // Soma +1 salto
        if (newCost > INFINITY_COST) newCost = INFINITY_COST;

        RouteEntry *entry = &routingTable[rIdx];

        // Se a rota ja era por esse vizinho, atualiza a metrica obrigatoriamente
        if (strcmp(entry->nextHop, senderIp) == 0) {
            if (newCost < INFINITY_COST) {
                entry->lastUpdated = now; // Renova o tempo de vida
            }
            if (entry->metric != newCost) {
                entry->metric = newCost;
                if (newCost == INFINITY_COST) {
                    applyKernelRoute(entry->subnet, entry->nextHop, 1); // Delete
                } else {
                    applyKernelRoute(entry->subnet, senderIp, 0); // Replace
                }
                changed = 1;
            }
        } 
        // Se a rota for por outro caminho mas esse vizinho oferece custo menor
        else if (newCost < entry->metric) {
            entry->metric = newCost;
            strncpy(entry->nextHop, senderIp, 20);
            entry->lastUpdated = now;
            applyKernelRoute(entry->subnet, senderIp, 0); // Replace
            printf("[%s] Rota dinamica aprendida: %s via %s (Saltos: %d)\n",
                   hostName, entry->subnet, senderIp, newCost);
            changed = 1;
        }
    }
    return changed;
}

// Varre a tabela buscando rotas que nao sao anunciadas ha muito tempo
int checkRouteTimeouts(time_t now) {
    int changed = 0;
    for (int i = 0; i < routeCount; i++) {
        if (!routingTable[i].directlyConnected && routingTable[i].metric < INFINITY_COST) {
            if (now - routingTable[i].lastUpdated >= DEAD_INTERVAL) {
                printf("[%s] Timeout na rota %s via %s (Custo -> 16)\n",
                       hostName, routingTable[i].subnet, routingTable[i].nextHop);
                routingTable[i].metric = INFINITY_COST;
                applyKernelRoute(routingTable[i].subnet, routingTable[i].nextHop, 1);
                strcpy(routingTable[i].nextHop, "0.0.0.0");
                changed = 1; // Ativa Triggered Update
            }
        }
    }
    return changed;
}

int main() {
    gethostname(hostName, sizeof(hostName));
    hostName[15] = '\0';

    discoverLocalInterfaces();

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct timeval tv = {0, 200000}; // 200ms de recv timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) < 0) {
        perror("Erro no bind UDP");
        return 1;
    }

    printf("[%s] Mini-RIP Dinamico ativo na porta UDP %d...\n", hostName, PORT);

    time_t lastSend = 0;
    while (1) {
        time_t now = time(NULL);
        int triggeredUpdate = 0;

        RipPacket recvPkt;
        struct sockaddr_in srcAddr;
        socklen_t addrLen = sizeof(srcAddr);

        // Se receber um pacote de roteamento...
        if (recvfrom(sockfd, &recvPkt, sizeof(recvPkt), 0, (struct sockaddr *)&srcAddr, &addrLen) > 0) {
            char senderIp[20];
            inet_ntop(AF_INET, &srcAddr.sin_addr, senderIp, sizeof(senderIp));

            // Ignora pacotes de broadcast refletidos do proprio roteador
            if (!isMyOwnIp(senderIp)) {
                if (processIncomingPacket(&recvPkt, senderIp, now)) {
                    triggeredUpdate = 1;
                }
            }
        }

        // Se alguma rota expirar por timeout...
        if (checkRouteTimeouts(now)) {
            triggeredUpdate = 1;
        }

        // Dispara anuncios caso tenha ocorrido Triggered Update ou atingiu o Timer normal de 1s
        if (triggeredUpdate || (now - lastSend >= UPDATE_INTERVAL)) {
            sendRoutingUpdates(sockfd);
            lastSend = now;
        }
    }

    close(sockfd);
    return 0;
}