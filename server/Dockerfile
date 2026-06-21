FROM python:3.14-slim
WORKDIR /app
ENV PYTHONUNBUFFERED=1

# Pillow needs JPEG/WebP/zlib at runtime. On armv7 there may be no prebuilt
# cp314 wheel, so include build deps + piwheels and compile as a fallback;
# purge the build-only deps in the same layer to keep the image small.
COPY requirements.txt ./
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libjpeg62-turbo libwebp7 zlib1g \
        gcc libjpeg-dev libwebp-dev zlib1g-dev \
    && grep -v -E '^(pytest|httpx|pytest-asyncio|ruff)' requirements.txt > requirements-runtime.txt \
    && pip install --no-cache-dir \
        --extra-index-url https://www.piwheels.org/simple \
        -r requirements-runtime.txt \
    && apt-get purge -y gcc libjpeg-dev libwebp-dev zlib1g-dev \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* requirements-runtime.txt

COPY main.py config.py database.py ./
COPY templates/ ./templates/
COPY static/ ./static/

ENV PHOTOFRAME_DATA_DIR=/data
RUN mkdir -p /data
EXPOSE 8080
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8080"]
