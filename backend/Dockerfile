FROM python:3.12-slim-bookworm

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl unzip \
    && rm -rf /var/lib/apt/lists/*

COPY backend/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt \
    && pip install --no-cache-dir playwright==1.49.1

# OS libraries only. This does not download a browser.
RUN playwright install-deps chromium \
    && rm -rf /var/lib/apt/lists/*

# Playwright 1.49.1 expects this exact revision. Its own downloader gives each
# CDN 30 seconds and, once PLAYWRIGHT_DOWNLOAD_HOST is set, does not fall back.
# Fetch the archive directly so a slow mirror can retry.
ENV PLAYWRIGHT_BROWSERS_PATH=/ms-playwright
RUN set -eu; \
    revision=1148; \
    dest="/ms-playwright/chromium-${revision}"; \
    mkdir -p "${dest}"; \
    ok=0; \
    for url in \
        "https://cdn.npmmirror.com/binaries/playwright/builds/chromium/${revision}/chromium-linux.zip" \
        "https://playwright.azureedge.net/builds/chromium/${revision}/chromium-linux.zip"; \
    do \
        echo "fetch ${url}"; \
        if curl -fL --retry 8 --retry-delay 5 --retry-all-errors \
            --connect-timeout 20 --max-time 900 \
            -o /tmp/chromium.zip "${url}"; then \
            ok=1; \
            break; \
        fi; \
        rm -f /tmp/chromium.zip; \
    done; \
    test "${ok}" = 1; \
    unzip -q /tmp/chromium.zip -d "${dest}"; \
    rm -f /tmp/chromium.zip; \
    test -x "${dest}/chrome-linux/chrome"; \
    chmod -R a+rX /ms-playwright

COPY backend/xhs_fetch.py backend/xhs_browser.py backend/xhs_state.py backend/xhs_server.py /app/

ENV XHS_DATA_DIR=/data \
    XHS_API_HOST=0.0.0.0 \
    XHS_API_PORT=8787 \
    XHS_CONSOLE_HOST=0.0.0.0 \
    XHS_CONSOLE_PORT=8790 \
    PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
    PYTHONUNBUFFERED=1

EXPOSE 8787 8790

CMD ["python", "-u", "xhs_server.py"]
