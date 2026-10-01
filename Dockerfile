FROM frrouting/frr:latest

RUN apk add --no-cache tcpdump gcc musl-dev

RUN touch /etc/frr/vtysh.conf