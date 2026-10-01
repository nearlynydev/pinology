# Emulator only. DSM PAT, boot artifacts, flash and disks stay outside the image.
FROM vdsm/virtual-dsm@sha256:02cea121099a65a75575c974d71a5898666cc324ab26ae8c04a04e0a42172181 AS extractor
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS build
# Build-stage xz alternatives refer to man pages. Keep those pages here so
# package configuration does not warn about missing targets; runtime stays slim.
RUN sed -i '\|^path-exclude /usr/share/man/|d' /etc/dpkg/dpkg.cfg.d/docker && \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential python3 python3-venv ninja-build meson pkg-config \
    libglib2.0-dev libfdt-dev zlib1g-dev curl xz-utils openssl \
    ca-certificates patch git
COPY build-slirp.sh build-qemu.sh /build/kernel/stock-board/
COPY qemu/ /build/kernel/stock-board/qemu/
ENV DSM_QEMU_WORK=/build/work DSM_QEMU_BUILD=/build/out DSM_QEMU_SLIRP=1 \
    MESON=/usr/bin/meson PKG_CONFIG_PATH=/build/work/libslirp-hostfwd/lib/pkgconfig
RUN bash /build/kernel/stock-board/build-slirp.sh && \
    bash /build/kernel/stock-board/build-qemu.sh

FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv python3-msgpack python3-pysodium qemu-utils libglib2.0-0t64 libfdt1 zlib1g ca-certificates && \
    python3 -m venv --system-site-packages /opt/pat-python && \
    /opt/pat-python/bin/pip install --no-cache-dir dissect.cstruct==4.7 && apt-get clean
COPY --from=build /build/out/qemu-system-aarch64 /opt/ds223/bin/qemu-system-aarch64
COPY --from=build /build/work/qemu-11.1.1/COPYING /usr/share/licenses/pinology/QEMU-COPYING
COPY --from=build /build/work/libslirp-v4.9.5/LICENSE /usr/share/licenses/pinology/libslirp-LICENSE
COPY --from=build /build/work/libslirp-v4.9.5/COPYRIGHT /usr/share/licenses/pinology/libslirp-COPYRIGHT
COPY licenses/ /usr/share/licenses/pinology/
COPY instance.py boot_from_flash.py lifecycle.py checkpoint.py instance-qmp.py settings.py profiles.py guest_control.py prepare_media.py flash-media.py vendor_identity.py storage.py lease.py /opt/ds223/runtime/
COPY --from=extractor /run/extract.py /opt/ds223/runtime/extract.py
COPY docker-entrypoint.sh /opt/ds223/bin/entrypoint
ENV STORAGE=/data ACCEL=tcg BIND_ADDRESS=0.0.0.0 SMB_PORT=14445
WORKDIR /data
HEALTHCHECK --interval=30s --timeout=10s --start-period=600s --retries=3 CMD /opt/pat-python/bin/python /opt/ds223/runtime/instance.py health || exit 1
ENTRYPOINT ["/bin/sh", "/opt/ds223/bin/entrypoint"]
CMD ["run"]
