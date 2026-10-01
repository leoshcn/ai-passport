FROM python:3.12-slim-bookworm

WORKDIR /app

COPY backend/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt \
    && pip install --no-cache-dir playwright==1.49.1 \
    && playwright install --with-deps chromium

COPY backend/xhs_fetch.py backend/xhs_browser.py backend/xhs_state.py backend/xhs_server.py /app/

ENV XHS_DATA_DIR=/data \
    XHS_API_HOST=0.0.0.0 \
    XHS_API_PORT=8787 \
    XHS_CONSOLE_HOST=0.0.0.0 \
    XHS_CONSOLE_PORT=8790 \
    PYTHONUNBUFFERED=1

EXPOSE 8787 8790

CMD ["python", "-u", "xhs_server.py"]
