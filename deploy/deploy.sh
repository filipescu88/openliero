#!/usr/bin/env bash
# Wdrożenie / aktualizacja Liero online na serwerze (Docker + Cloudflare Tunnel).
#
#   ./deploy/deploy.sh            najnowszy kod + gotowe obrazy z GitHuba, restart
#   ./deploy/deploy.sh --build    obrazy budowane tutaj, na serwerze (wolniej)
#   ./deploy/deploy.sh --no-pull  bez `git pull` (wdraża to, co jest na dysku)
#
# Pierwsze uruchomienie pyta o domenę i token tunelu Cloudflare i zapisuje je
# w deploy/.env (tylko na tym serwerze, nigdy w repozytorium).
set -euo pipefail

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$DEPLOY_DIR")"
ENV_FILE="$DEPLOY_DIR/.env"

build=0
git_pull=1
for arg in "$@"; do
	case "$arg" in
	--build) build=1 ;;
	--no-pull) git_pull=0 ;;
	-h | --help)
		sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "Nieznana opcja: $arg (zobacz --help)" >&2
		exit 2
		;;
	esac
done

step() { printf '\n\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mUwaga:\033[0m %s\n' "$*" >&2; }
fail() {
	printf '\033[1;31mBłąd:\033[0m %s\n' "$*" >&2
	exit 1
}

compose() { docker compose --project-directory "$DEPLOY_DIR" -f "$DEPLOY_DIR/docker-compose.yml" "$@"; }

# --- 1. Narzędzia ------------------------------------------------------------
step "Sprawdzam narzędzia"
for tool in docker git curl; do
	command -v "$tool" >/dev/null 2>&1 || fail "brak programu '$tool' — zainstaluj go i uruchom skrypt ponownie."
done
docker compose version >/dev/null 2>&1 || fail "brak 'docker compose' (wtyczka Compose v2)."
docker info >/dev/null 2>&1 || fail "Docker nie działa albo nie masz do niego uprawnień (spróbuj: sudo usermod -aG docker \$USER, potem wyloguj się i zaloguj)."

# --- 2. Ustawienia (deploy/.env) ---------------------------------------------
if [[ ! -f "$ENV_FILE" ]]; then
	step "Pierwsze uruchomienie: ustawienia"
	[[ -t 0 ]] || fail "brak $ENV_FILE, a skrypt nie działa w terminalu, więc nie może zapytać. Skopiuj deploy/.env.example do deploy/.env i uzupełnij."
	read -r -p "Domena gry [liero.pl]: " domain
	domain="${domain:-liero.pl}"
	domain="${domain#http://}"
	domain="${domain#https://}"
	domain="${domain%%/*}"
	token=""
	while [[ -z "$token" ]]; do
		read -r -s -p "Token tunelu Cloudflare (nie będzie widoczny przy wpisywaniu): " token
		echo
		# Wklejona cała komenda z panelu Cloudflare (`cloudflared service install
		# <token>`, `... run --token <token>`)? Token to zawsze ostatnie słowo.
		token="$(printf '%s' "$token" | awk '{ print $NF }')"
	done
	(
		umask 077
		sed -e "s|^SITE_DOMAIN=.*|SITE_DOMAIN=${domain}|" \
			-e "s|^TUNNEL_TOKEN=.*|TUNNEL_TOKEN=${token}|" \
			"$DEPLOY_DIR/.env.example" >"$ENV_FILE"
	)
	echo "Zapisano $ENV_FILE (tylko na tym serwerze)."
fi

# shellcheck disable=SC1090
set -a && source "$ENV_FILE" && set +a
[[ -n "${SITE_DOMAIN:-}" ]] || fail "brak SITE_DOMAIN w $ENV_FILE"
[[ -n "${TUNNEL_TOKEN:-}" ]] || fail "brak TUNNEL_TOKEN w $ENV_FILE"
WEB_PORT="${WEB_PORT:-8088}"

# --- 3. Najnowszy kod --------------------------------------------------------
if [[ "$git_pull" == 1 ]]; then
	step "Pobieram najnowszy kod (git pull)"
	before="$(sha256sum "${BASH_SOURCE[0]}")"
	git -C "$REPO_DIR" pull --ff-only ||
		fail "git pull się nie udał (masz lokalne zmiany? sprawdź: git -C $REPO_DIR status)."
	if [[ "$(sha256sum "${BASH_SOURCE[0]}")" != "$before" ]]; then
		echo "Skrypt wdrożenia się zmienił — uruchamiam nową wersję."
		exec "${BASH_SOURCE[0]}" --no-pull "$@"
	fi
fi

# --- 4. Obrazy ---------------------------------------------------------------
if [[ "$build" == 1 ]]; then
	step "Buduję obrazy na serwerze (pierwszy raz może to potrwać kilkanaście minut)"
	compose build --pull
else
	step "Pobieram gotowe obrazy"
	if ! compose pull; then
		if docker image inspect "${RELAY_IMAGE:-ghcr.io/filipescu88/openliero/relay-server:latest}" >/dev/null 2>&1 &&
			docker image inspect "${WEB_IMAGE:-ghcr.io/filipescu88/openliero/web:latest}" >/dev/null 2>&1; then
			warn "nie udało się pobrać nowych obrazów — uruchamiam te, które już są na serwerze."
		else
			fail "nie udało się pobrać obrazów. Jeśli repozytorium jest prywatne, zaloguj się raz:
    docker login ghcr.io -u <login-github>
(hasło: token GitHuba z uprawnieniem read:packages). Albo zbuduj wszystko tutaj: $0 --build"
		fi
	fi
fi

# --- 5. Start ----------------------------------------------------------------
step "Uruchamiam kontenery"
if [[ "$build" == 1 ]]; then
	compose up -d --remove-orphans
else
	compose up -d --remove-orphans --no-build
fi

# --- 6. Sprawdzenie ----------------------------------------------------------
step "Sprawdzam, czy gra odpowiada"
ok=0
for _ in $(seq 1 30); do
	if curl -fsS "http://127.0.0.1:${WEB_PORT}/healthz" >/dev/null 2>&1 &&
		curl -fsS "http://127.0.0.1:${WEB_PORT}/" 2>/dev/null | grep -q "openliero.js"; then
		ok=1
		break
	fi
	sleep 2
done
if [[ "$ok" != 1 ]]; then
	compose ps
	compose logs --tail 30 web relay
	fail "gra nie odpowiada na http://127.0.0.1:${WEB_PORT} (logi powyżej)."
fi
echo "Strona i serwer pokoi działają lokalnie."

tunnel=0
for _ in $(seq 1 15); do
	if compose logs cloudflared 2>&1 | grep -q "Registered tunnel connection"; then
		tunnel=1
		break
	fi
	sleep 2
done
if [[ "$tunnel" == 1 ]]; then
	echo "Tunel Cloudflare połączony."
elif compose logs cloudflared 2>&1 | grep -q "token is not valid"; then
	warn "Cloudflare odrzuca token tunelu. Skopiuj go jeszcze raz z panelu (Zero Trust → Networks → Tunnels) i popraw TUNNEL_TOKEN w $ENV_FILE, potem uruchom skrypt ponownie."
else
	compose logs --tail 15 cloudflared || true
	warn "tunel Cloudflare jeszcze się nie połączył (logi powyżej). Podgląd na żywo: docker compose -f $DEPLOY_DIR/docker-compose.yml logs -f cloudflared"
fi

# --- 7. Sprzątanie -----------------------------------------------------------
docker image prune -f >/dev/null || true

compose ps --format 'table {{.Service}}\t{{.Status}}'
if [[ "$tunnel" == 1 ]]; then
	step "Gotowe: https://${SITE_DOMAIN}"
else
	step "Gra działa na serwerze (http://127.0.0.1:${WEB_PORT}), ale nie jest jeszcze widoczna z internetu — patrz uwaga o tunelu powyżej."
	exit 1
fi
