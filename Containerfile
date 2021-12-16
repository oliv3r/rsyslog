# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (C) 2022 Olliver Schinagl <oliver@schinagl.nl>

FROM index.docker.io/library/alpine:latest AS builder

WORKDIR /src

COPY . /src/

RUN apk add --no-cache \
        autoconf \
        autoconf-archive \
        automake \
        binutils \
        bison \
        bsd-compat-headers \
        curl-dev \
        czmq-dev \
        file \
        flex \
        gcc \
        glib-dev \
        gnutls-dev \
        hiredis-dev \
        krb5-dev \
        libc-dev \
        libdbi-dev \
        libestr-dev \
        libevent-dev \
        libfastjson-dev \
        libgcrypt-dev \
        liblogging-dev \
        liblognorm-dev \
        libmaxminddb-dev \
        libnet-dev \
        libpq-dev \
        librdkafka-dev \
        librelp-dev \
        libtool \
        linux-headers \
        make \
        mariadb-connector-c-dev \
        mongo-c-driver-dev \
        musl-dev \
        net-snmp-dev \
        openssl-dev \
        py3-docutils \
        python3 \
        rabbitmq-c-dev \
        tcl-dev \
        util-linux-dev \
        zlib-dev \
    && \
    autoreconf --force --install && \
    ./configure \
                --prefix='/usr' \
                --sysconfdir='/etc' \
                --disable-libsystemd \
                --disable-unlimited-select \
                --disable-generate-man-pages \
                --disable-imsolaris \
                --enable-atomic_operations \
                --enable-clickhouse \
                --enable-debug \
                --enable-default-tests \
                --enable-elasticsearch \
                --disable-ffaup \
                --enable-fmhash \
                --enable-fmhash-xxhash \
                --enable-fmhttp \
                --enable-fmunflatten \
                --enable-gnutls \
                --enable-gssapi-krb5 \
                --enable-helgrind \
                --enable-imbatchreport \
                --enable-imczmq \
                --enable-imdiag \
                --enable-imdocker \
                --enable-imfile \
                --enable-imfile-tests \
                --enable-imhiredis \
                --enable-imkafka \
                --enable-improg \
                --enable-impstats \
                --enable-imptcp \
                --enable-imtuxedoulog \
                --enable-inet \
                --enable-klog \
                --enable-kmsg \
                --enable-largefile \
                --enable-libdbi \
                --enable-libgcrypt \
                --enable-mail \
                --enable-mmanon \
                --enable-mmaudit \
                --enable-mmcount \
                --enable-mmdarwin \
                --enable-mmdblookup \
                --enable-mmfields \
                --disable-mmgrok \
                --enable-mmjsonparse \
                --enable-mmkubernetes \
                --enable-mmnormalize \
                --enable-mmpstrucdata \
                --enable-mmrfc5424addhmac \
                --enable-mmrm1stspace \
                --enable-mmsequence \
                --enable-mmsnmptrapd \
                --enable-mmtaghostname \
                --enable-mmutf8fix \
                --enable-mysql \
                --enable-omczmq \
                --enable-omfile-hardened \
                --disable-omhdfs \
                --enable-omhiredis \
                --disable-omhttp \
                --disable-omhttpfs \
                --disable-omkafka \
                --disable-ommongodb \
                --disable-omprog \
                --disable-omrabbitmq \
                --disable-omruleset \
                --disable-omstdout \
                --disable-omtcl \
                --disable-omudpspoof \
                --disable-omuxsock \
                --disable-openssl \
                --disable-pgsql \
                --disable-pmaixforwardedfrom \
                --disable-pmciscoios \
                --disable-pmcisconames \
                --disable-pmdb2diag \
                --disable-pmlastmsg \
                --disable-pmnormalize \
                --disable-pmnull \
                --disable-pmpanngfw \
                --disable-pmsnare \
                --disable-regexp \
                --disable-relp \
                --disable-rfc3195 \
                --disable-rsyslogd \
                --disable-rsyslogrt \
                --disable-snmp \
                --disable-uuid \
        && \
        make && \
        make DESTDIR="/rsyslog" install && \
        find '/rsyslog' -iname '*.la' -delete


# FROM index.docker.io/library/alpine:latest
#
# LABEL maintainer="Olliver Schinagl <oliver@schinagl.nl>"
#
# RUN apk add --no-cache \
#         nodejs-current \
#     && \
#     addgroup -S 'jshint' && \
#     adduser -D -G 'jshint' -h '/var/lib/jshint' -s '/bin/false' -S 'jshint' && \
#     ln -f -n -s '/usr/local/lib/node_modules/jshint/bin/jshint' '/usr/local/bin/jshint'
#
# COPY --from=builder "/jshint/node_modules/" "/usr/local/lib/node_modules"
# COPY "./containerfiles/container-entrypoint.sh" "/init"
#
# ENTRYPOINT [ "/init" ]
#
# USER 'jshint'
