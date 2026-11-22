# SPDX-License-Identifier: MPL-2.0
# The Aurora68 music server (aurora68_music_server.py): a folder of MP3s as internet radio stations,
# over IPv4 and IPv6. Python's standard library and ffmpeg (for the formats that are not MP3), on Alpine.
#   docker build -t aurora68-music-server tools/music-server
#   docker run -d --name aurora68-music --restart unless-stopped --network host \
#       -v /path/to/music:/music:ro aurora68-music-server
# --network host gives it the NAS's own IPv4 and IPv6 addresses; Docker's bridge network has no IPv6
# unless the daemon is set up for it. The music is mounted read-only: the server never writes.
FROM python:3.12-alpine
LABEL org.opencontainers.image.title="Aurora68 music server" \
      org.opencontainers.image.description="A folder of MP3s as internet radio stations, IPv4 and IPv6" \
      org.opencontainers.image.source="https://github.com/farsonic/Aurora68" \
      org.opencontainers.image.licenses="MPL-2.0"
# ffmpeg converts FLAC, M4A (AAC and ALAC), Ogg, Opus, WAV and AIFF to MP3 as they are sent
RUN apk add --no-cache ffmpeg
COPY aurora68_music_server.py /app/aurora68_music_server.py
# settings: MUSIC_NAME (the library's name, "Music"), MUSIC_PORT (8000), MUSIC_BIND ("::", every address,
# IPv6 and IPv4), MUSIC_MAX_LISTENERS (16), MUSIC_BITRATE (192: what other formats become); the music at /music
ENV MUSIC_DIR=/music MUSIC_PORT=8000 PYTHONUNBUFFERED=1
EXPOSE 8000
# any user that can read the music: nobody by default, the owner's PUID:PGID from docker-compose.yml
USER 65534:65534
HEALTHCHECK --interval=60s --timeout=5s \
  CMD python3 -c "import os,urllib.request; urllib.request.urlopen('http://127.0.0.1:%s/api/list' % os.environ['MUSIC_PORT'], timeout=4)"
ENTRYPOINT ["python3", "/app/aurora68_music_server.py"]
