# SPDX-License-Identifier: MPL-2.0
# The Aurora68 video card's helper in a container (docs/16-video-card.md): Python, ffmpeg and yt-dlp.
#   docker build -t aurora68-video-helper tools/video-helper
#   docker run -d --restart unless-stopped --name aurora68-video --network host aurora68-video-helper
# --network host (Linux) lets the Mac's discovery broadcast reach it; with -p 8069:8069 instead (Docker
# Desktop on a Mac or Windows) the Mac is given the computer's address by hand.
# YouTube changes often: rebuild (or "docker exec -u root aurora68-video pip install -U yt-dlp") when searches fail.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir yt-dlp
COPY aurora68_video_helper.py /usr/local/bin/aurora68_video_helper.py
# settings: AURORA68_VIDEO_PORT (8069), AURORA68_VIDEO_NAME (the name the Mac shows), AURORA68_VIDEO_TOKEN (none)
ENV AURORA68_VIDEO_NAME=docker
EXPOSE 8069/tcp 8069/udp
USER nobody
ENTRYPOINT ["python3", "-u", "/usr/local/bin/aurora68_video_helper.py"]
