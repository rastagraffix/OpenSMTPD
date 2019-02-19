FROM alpine:3.9 as build

WORKDIR /opensmtpd

RUN apk add --no-cache \
    ca-certificates \
    wget \
    cmake \
    automake \
    autoconf \
    libtool \
    gcc \
    make \
    musl-dev \
    bison \
    libevent-dev \
    libtool \
    libasr-dev \
    fts-dev \
    zlib-dev \
    libressl-dev

COPY . /opensmtpd

#build opensmtpd
RUN rm -r /usr/local/
RUN ./bootstrap && \
    ./configure --with-gnu-ld --sysconfdir=/etc --with-path-empty=/var/lib/opensmtpd/empty/ && \
    make && \
    make install

FROM alpine:3.9
LABEL maintainer="Arthur Moore <Arthur.Moore.git@cd-net.net>"

EXPOSE 25
EXPOSE 465
EXPOSE 587

VOLUME /var/spool/smtpd
WORKDIR /var/spool/smtpd

ENTRYPOINT ["smtpd", "-d"]
CMD ["-P", "mda"]

RUN apk add --no-cache libressl libevent libasr fts zlib ca-certificates && \
    mkdir -p /var/lib/opensmtpd/empty/ && \
    adduser _smtpd -h /var/lib/opensmtpd/empty/ -D -H -s /bin/false && \
    adduser _smtpq -h /var/lib/opensmtpd/empty/ -D -H -s /bin/false && \
    mkdir -p /var/spool/smtpd

COPY --from=build /usr/local/ /usr/local/

COPY smtpd/smtpd.conf /etc/

#OpenSMTPD needs root permissions to open port 25.
#It immediately changes to running as _smtpd after that.
