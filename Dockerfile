FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG MAKE_TARGET=build-release
ARG VPP_BUILD_JOBS=0
ARG VPP_GIT_TAG_FALLBACK=v25.02
ARG VPP_GIT_DESCRIBE_FALLBACK=v25.02-0-gcontainer

ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV UNATTENDED=y

WORKDIR /src/vpp

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    make \
    sudo \
 && rm -rf /var/lib/apt/lists/*

COPY . /src/vpp

RUN printf 'SOURCE_PATH = /src/vpp\n' > /src/vpp/build-root/build-config.mk \
 && if git -C /src/vpp describe --long --match 'v*' > /tmp/vpp-version 2>/dev/null; then \
      cp /tmp/vpp-version /src/vpp/src/.version; \
    else \
      printf '%s\n' "$VPP_GIT_DESCRIBE_FALLBACK" > /src/vpp/src/.version; \
      rm -rf /src/vpp/.git; \
      git -C /src/vpp init; \
      git -C /src/vpp add -A; \
      git -C /src/vpp \
        -c user.name='Docker Build' \
        -c user.email='docker-build@example.invalid' \
        commit -m 'Container snapshot'; \
      git -C /src/vpp tag "$VPP_GIT_TAG_FALLBACK"; \
    fi

RUN make install-dep

RUN if [ "$VPP_BUILD_JOBS" = "0" ]; then \
      make "$MAKE_TARGET"; \
    else \
      make -j"$VPP_BUILD_JOBS" "$MAKE_TARGET"; \
    fi

CMD ["/bin/bash"]
