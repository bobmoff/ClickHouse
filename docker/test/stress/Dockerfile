# rebuild in #33610
# docker build -t clickhouse/stress-test .
ARG FROM_TAG=latest
FROM clickhouse/stateful-test:$FROM_TAG

RUN apt-get update -y \
    && env DEBIAN_FRONTEND=noninteractive \
        apt-get install --yes --no-install-recommends \
            bash \
            tzdata \
            parallel \
            expect \
            python3 \
            python3-lxml \
            python3-termcolor \
            python3-requests \
            curl \
            sudo \
            openssl \
            netcat-openbsd \
            brotli \
    && apt-get clean

COPY run.sh /

ENV EXPORT_S3_STORAGE_POLICIES=1

CMD ["/bin/bash", "/run.sh"]
