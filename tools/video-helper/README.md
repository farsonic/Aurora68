# The Aurora68 video card's helper

The helper does the part of video the Pi cannot: it finds videos (YouTube, through yt-dlp) and turns them
into MPEG-1 with MP2 sound (ffmpeg). It is a small web server on any computer on the same network as the
SE. The Mac's program Aurora68 Video finds it, asks it for searches and streams over the Mac's own TCP/IP
(MacTCP or Riptide, as Aurora68 Play plays radio), and hands the stream to the video card in the Pi, which
decodes it into the Mac's window. The Pi's remote control is not involved and can stay off. Design:
`docs/16-video-card.md`.

It listens on port 8069: HTTP on TCP, and on UDP it answers the Mac's discovery broadcast, so nothing needs
to be typed on the Mac.

## On a Mac
```bash
brew install ffmpeg yt-dlp
```
```bash
python3 aurora68_video_helper.py
```
Leave it running. The first time, macOS asks whether Python may accept incoming connections: allow it.
`--name Study` changes the name the Mac shows (the computer's name by default).

## In Docker
```bash
docker build -t aurora68-video-helper tools/video-helper
```
```bash
docker run -d --restart unless-stopped --name aurora68-video --network host aurora68-video-helper
```
Or `docker compose up -d` in this folder. `--network host` (Linux) lets the Mac's discovery broadcast reach
the container. Docker Desktop on a Mac or Windows does not pass broadcasts in: use `-p 8069:8069 -p
8069:8069/udp` instead and give Aurora68 Video the computer's address (File > Helper...). `docker logs
aurora68-video` shows what it is doing.

## Settings
| Option | Environment | |
|---|---|---|
| `--port N` | `AURORA68_VIDEO_PORT` | TCP and UDP port, 8069 |
| `--name NAME` | `AURORA68_VIDEO_NAME` | the name the Mac shows |
| `--token SECRET` | `AURORA68_VIDEO_TOKEN` | refuse clients that do not send it (the Mac is given the same token); off by default |
| `--bind ADDR` | | listen on one address only (all by default) |
| `--file F` | | file mode: only these files, no YouTube (repeat for more) |
| `--yt-dlp P`, `--ffmpeg P` | | where the two programs are, if not on the PATH |

## Without YouTube
`--file movie.mp4 --file another.mkv` serves only those files: they are what a search finds. Any format
ffmpeg reads.

## When YouTube stops working
YouTube changes the way its streams are unlocked now and then, and yt-dlp follows within days:
`brew upgrade yt-dlp` on a Mac, `docker exec -u root aurora68-video pip install -U yt-dlp` (or rebuild the
image) in Docker. The helper reports yt-dlp's own error to the Mac in words.

## Test
`python3 tools/video-helper/test_helper.py` (needs ffmpeg) makes a test movie and does what the Mac will:
the discovery probe, `/info`, `/search`, `/play` (checked with ffprobe: MPEG-1 video at the size asked for,
MP2 at 44.1 kHz stereo), a play stopped early leaving no ffmpeg behind, refusals, the token.
