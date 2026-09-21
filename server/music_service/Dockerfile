FROM node:22-bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY src ./src

RUN useradd --create-home --uid 10003 music \
    && mkdir -p /data \
    && chown -R music:music /app /data

USER music
EXPOSE 8788
CMD ["node", "src/server.js"]
