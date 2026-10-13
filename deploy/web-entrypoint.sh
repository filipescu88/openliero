#!/bin/sh
# Starts the "web" container: writes the game page with the site's public
# address filled in (link previews need absolute URLs), then runs Caddy.
set -eu

site="${SITE_URL:-}"
site="${site%/}"
for page in index.html openliero.html; do
	sed "s|__SITE_URL__|${site}|g" /srv/page.template.html >"/srv/${page}"
done

exec caddy run --config /etc/caddy/Caddyfile --adapter caddyfile
