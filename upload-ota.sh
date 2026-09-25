#!/usr/bin/env bash
# Sube el firmware por WiFi (OTA) a uno o varios controladores.
#   ./upload-ota.sh carpa-1 carpa-2 192.168.1.202   → a esos equipos
#   ./upload-ota.sh                                  → busca los equipos de la red y pregunta
set -u

PIO=${PIO:-~/.platformio/penv/bin/pio}  # Se puede cambiar: PIO=/otra/ruta/pio ./upload-ota.sh

cd "$(dirname "$0")"

hosts=()
labels=()

if [ $# -gt 0 ]; then
  for h in "$@"; do
    # Nombre sin dominio → <nombre>.local; las IPs quedan igual
    if [[ "$h" != *.* ]]; then h="$h.local"; fi
    hosts+=("$h")
    labels+=("$h")
  done
else
  echo "Buscando controladores en la red..."
  if command -v avahi-browse >/dev/null; then
    # Por mDNS: rápido y encuentra solo los controladores
    while IFS=';' read -r _ _ proto name _ _ _ ip _; do
      [ "$proto" = "IPv4" ] || continue
      hosts+=("$ip")
      labels+=("$name ($ip)")
    done < <(avahi-browse -rtp _fungi._tcp 2>/dev/null | grep '^=' | sort -u -t';' -k8,8)
  else
    # Sin avahi-utils: se pregunta /device a cada IP de la red local (/24)
    base=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}' | cut -d. -f1-3)
    if [ -z "$base" ]; then
      echo "No se pudo detectar la red local. Pasá los equipos a mano: ./upload-ota.sh carpa-1 carpa-2"
      exit 1
    fi
    while IFS=';' read -r name ip; do
      hosts+=("$ip")
      labels+=("$name ($ip)")
    done < <(seq 1 254 | xargs -P 64 -I{} sh -c '
      body=$(curl -s -m 2 "http://'"$base"'.{}/device") || exit 0
      name=$(printf "%s" "$body" | sed -n "s/.*\"name\":\"\([^\"]*\)\".*/\1/p")
      printf "%s" "$body" | grep -q "\"id\":" || exit 0
      [ -n "$name" ] && printf "%s;%s\n" "$name" "'"$base"'.{}"
    ' | sort -t';' -k1,1)
  fi

  if [ ${#hosts[@]} -eq 0 ]; then
    echo "No se encontró ningún controlador. Revisá que estén prendidos y en la misma WiFi."
    exit 1
  fi
  echo "Encontrados:"
  printf '  - %s\n' "${labels[@]}"
  read -r -p "¿Subir el firmware a todos? [s/N] " answer
  [[ "$answer" =~ ^[sS]$ ]] || { echo "Cancelado."; exit 0; }
fi

echo "Compilando..."
"$PIO" run -e ota || { echo "Error de compilación, no se subió nada."; exit 1; }

results=()
failed=0
for i in "${!hosts[@]}"; do
  echo
  echo "=== Subiendo a ${labels[$i]} ==="
  if "$PIO" run -e ota -t nobuild -t upload --upload-port "${hosts[$i]}"; then
    results+=("OK     ${labels[$i]}")
  else
    results+=("ERROR  ${labels[$i]}")
    failed=1
  fi
done

echo
echo "Resumen:"
printf '  %s\n' "${results[@]}"
exit $failed
