# Liero online na własnym serwerze

Gra w przeglądarce (telefony) + serwer pokoi, w kontenerach Dockera, wystawione
na świat przez **Cloudflare Tunnel**: bez otwierania portów na routerze, bez
certyfikatów, działa też przy zmiennym IP.

```
telefon ──https/wss──▶ Cloudflare ──tunel──▶ cloudflared ──▶ web (Caddy + gra) ──/ws──▶ relay
```

| Plik | Co robi |
|---|---|
| `deploy.sh` | jedyna komenda do wdrożenia i aktualizacji |
| `docker-compose.yml` | trzy kontenery: `relay`, `web`, `cloudflared` |
| `web.Dockerfile`, `Caddyfile`, `web-entrypoint.sh` | obraz z grą w wersji web |
| `.env.example` | wzór ustawień; prawdziwe są w `deploy/.env` (tylko na serwerze) |

Obrazy budują GitHub Actions po każdym merge do `master`
(`.github/workflows/web-image.yml` i `relay-server.yml`), więc serwer niczego
nie kompiluje.

## Pierwsze uruchomienie

1. **Domena w Cloudflare.** Na dash.cloudflare.com: *Add a domain* → `liero.pl`
   → plan *Free*. Cloudflare poda dwa serwery nazw; ustaw je u rejestratora
   domeny `.pl` zamiast obecnych. Poczekaj, aż domena będzie *Active*
   (dla `.pl` od kilku godzin do 1–2 dni).
2. **Tunel.** Cloudflare → *Zero Trust* → *Networks* → *Tunnels* →
   *Create a tunnel* → *Cloudflared* → nazwa np. `liero`. Na ekranie instalacji
   skopiuj komendę z tokenem (np. `cloudflared service install eyJ...`) — skrypt
   wyciągnie z niej token sam. Niczego z tej komendy nie instaluj.
3. **Adres w tunelu.** W tym samym tunelu, *Public hostname* → *Add*:
   subdomena pusta, domena `liero.pl`, *Service*: `HTTP`, URL: `web:80`.
   (Opcjonalnie drugi wpis dla `www`.)
4. **Docker na serwerze** (jeśli go nie ma):
   ```sh
   curl -fsSL https://get.docker.com | sh
   sudo usermod -aG docker "$USER"   # potem wyloguj się i zaloguj ponownie
   ```
5. **Dostęp do repozytorium.** Repo jest prywatne, więc serwer potrzebuje tokena
   GitHuba: github.com → *Settings* → *Developer settings* → *Personal access
   tokens* → *Tokens (classic)* → zakresy `repo` i `read:packages`. Potem:
   ```sh
   git clone https://github.com/filipescu88/openliero.git ~/openliero
   # login: Twój login GitHuba, hasło: token
   docker login ghcr.io -u <login-github>
   # hasło: ten sam token
   ```
6. **Start:**
   ```sh
   cd ~/openliero && ./deploy/deploy.sh
   ```
   Za pierwszym razem zapyta o domenę i token tunelu. Na końcu wypisze
   `Gotowe: https://liero.pl`.

## Aktualizacja

```sh
cd ~/openliero && ./deploy/deploy.sh
```

Skrypt pobiera najnowszy kod i obrazy, restartuje kontenery, sprawdza, czy gra
odpowiada i czy tunel jest połączony, i sprząta stare obrazy.

- `./deploy/deploy.sh --build` — buduje obrazy na serwerze zamiast pobierać
  (gdy GitHub Actions nie działa; pierwszy raz kilkanaście minut).
- `./deploy/deploy.sh --no-pull` — bez `git pull`.

## Gdy coś nie działa

```sh
cd ~/openliero/deploy
docker compose ps                      # stan kontenerów
docker compose logs -f cloudflared     # tunel
docker compose logs -f relay web       # serwer pokoi i strona
curl http://127.0.0.1:8088/healthz     # czy serwer pokoi żyje (z serwera)
```

- *Provided Tunnel token is not valid* — popraw `TUNNEL_TOKEN` w `deploy/.env`.
- Strona działa, ale łączenie z pokojem nie — sprawdź, czy `SITE_DOMAIN`
  w `deploy/.env` to dokładnie domena, pod którą otwierasz grę.
