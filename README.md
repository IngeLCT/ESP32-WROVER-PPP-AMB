# ESP32‑WROVER‑PPP‑AMB — Conexión PPP (A7670/SIM7600) + Firebase — ESP‑IDF 5.5.1

> **Estado actual**: este proyecto **sí usa** `esp_modem` para dar Internet al ESP32 vía **PPP sobre módem LTE (A7670/SIM7600)** y después conecta a **Firebase**. Aún **no** está integrado el bloque de **AT+CPSI?** para geolocalización por celda (eso va en la siguiente iteración).

---

## 1) Resumen
- **Objetivo**: dar conectividad IP al ESP32 por PPP (módem 4G) y usarla para autenticarse y enviar datos a **Firebase**.
- **Framework**: ESP‑IDF **v5.5.1**.
- **Componente de módem**: `espressif/esp_modem` (v1.4.x o similar) vía Component Manager.
- **Modo de operación**: **PPP DATA (CMUX desactivado)** por estabilidad. Si más adelante se habilita CMUX, se hará con *fallback* automático.
- **Notas**:
  - Se marca la interfaz PPP como **default** y se **fijan DNS** si el APN no provee, para evitar errores `getaddrinfo() 202`.
  - Sensores/I²C se inicializan **después** de levantar PPP (evita NACKs por picos de consumo del módem).

---

## 2) Hardware de referencia (T‑A7670X / SIM7600 compatible)
- **UART del módem**: `TX=GPIO26`, `RX=GPIO27`
- **Flow control**: **sin RTS/CTS** (puede activarse en el futuro si se requiere mayor throughput/TLS estable)
- **Líneas de control**:
  - `DTR = GPIO25`
  - `RST = GPIO5` (activo **LOW**)
  - `PWRKEY = GPIO4`
  - `BOARD_POWERON = GPIO12`
- **APN**: `internet.itelcel.com` (Telcel MX; sin user/pass)
- **Alimentación**: fuente capaz de entregar picos >2A al módem. Se recomiendan capacitores de *bulk* (≥470–1000 µF) cerca del módem/sensores.

> Ajusta pines/APN a tu hardware/operador en `modem_ppp.c`/`main.c`.

---

## 3) Dependencias
- ESP‑IDF 5.5.1 configurado (`idf.py doctor`).
- Componentes:
  - `espressif/esp_modem`
  - `esp_netif`, `esp_http_client`, `esp_crt_bundle`, `json` (cJSON)
  - Librería de **Firebase** (ya integrada en el proyecto: inicialización y autenticación por email/password)

Para añadir esp_modem (si no está bloqueado ya en `dependencies.lock`):
```bash
idf.py add-dependency "espressif/esp_modem^1.4.0"
```

---

## 4) Configuración (sdkconfig)
En `sdkconfig.defaults` asegúrate de:
```ini
CONFIG_LWIP_PPP_SUPPORT=y
# Opcional, para diagnóstico
# CONFIG_LWIP_PPP_DEBUG_ON=y
```
Luego reconfigura una vez:
```bash
idf.py reconfigure
```

---

## 5) Estructura relevante
```
main/
├─ main.c                 # app_main: arranca PPP, luego SNTP/sensores/Firebase
├─ modem_ppp.h/.c         # Encendido HW + DTE/DCE + PPP (DATA) + DNS fallback
├─ unwiredlabs.h/.c       # (presente) geoloc por celda — **AÚN NO** llamado
├─ Privado.h              # (no versionar) credenciales/constantes sensibles
└─ CMakeLists.txt         # registra SRCS y REQUIRES (esp_modem, etc.)
```

---

## 6) Flujo de arranque en `app_main`
1. `esp_netif_init()` y `esp_event_loop_create_default()`.
2. `modem_ppp_start_blocking(...)` con `.use_cmux = false` (DATA). Este paso:
   - Hace la **secuencia de encendido** del módem (POWERON/RST/PWRKEY/DTR)
   - Crea `esp_netif` PPP y lo marca como **default**
   - Crea DTE/DCE (`esp_modem_api`) con APN configurado
   - Sube **PPP** y espera `IP_EVENT_PPP_GOT_IP`
   - **Verifica DNS**; si no hay, **fuerza** `1.1.1.1` y `8.8.8.8`
3. (Opcional) SNTP/RTC.
4. Inicialización de **sensores/I²C** (con un `vTaskDelay(1000)` previo recomendable).
5. **Firebase**: autenticación con email/password y operaciones (push/set/remove) según lógica de la app.

> Si más adelante se activa **CMUX**, el código intentará CMUX y si falla hará *fallback* a DATA.

---

## 7) Credenciales y seguridad
- Usa `Privado.h` (ignorado en git) para claves sensibles: API key de Firebase, email/password, token de servicios externos.
- **No** loguees `post_field` ni credenciales en el monitor serie.
- Considera **rotar** la contraseña si apareció alguna vez en logs/commits.

---

## 8) Compilación, flasheo y monitor
```bash
idf.py fullclean
idf.py build
idf.py flash monitor
```
**Salida esperada mínima**:
- `PPP UP  ip=... gw=...`
- Mensajes de conexión/éxito de Firebase (token adquirido).

---

## 9) Troubleshooting rápido
- **`getaddrinfo() 202` / DNS falla**:
  - Asegura que PPP sea **default** (`esp_netif_set_default_netif(ppp)`).
  - Si el APN no entrega DNS, el módulo PPP del proyecto **fuerza** `1.1.1.1`/`8.8.8.8`.
- **Reinicios/errores CMUX**:
  - Mantén **CMUX desactivado**; usa `esp_modem_pause_net()` para enviar AT de forma puntual.
  - Si luego activas CMUX: sube `baudrate` y usa **RTS/CTS** si el hardware lo permite.
- **I²C NACK tras levantar módem**:
  - Añade `vTaskDelay(500–1500 ms)` antes de inicializar sensores;
  - Mejora la **alimentación** (picos) y verifica *pull-ups* de I²C.
- **TLS/HTTP fallan**:
  - Comprueba **hora** (SNTP) y `esp_crt_bundle` habilitado;
  - Prueba primero HTTP plano a `http://neverssl.com/` para aislar TLS.
- **Sin IP PPP**:
  - Verifica **APN** correcto, señal y SIM (PIN)
  - Aumenta LOG a `esp_modem` y `ppp` para ver negociaciones LCP/IPCP.

---

## 10) Roadmap inmediato
- ✅ Establecer conexión a **Firebase** usando PPP.
- ⏭️ Añadir **AT+CPSI?** + `AT+COPS?/AT+CEREG?` como respaldo, con pausa de red (`esp_modem_pause_net`) y envío a **Unwired Labs** para obtener ciudad/estado/fecha/hora. (Código helper ya incluido en `unwiredlabs.c`, pendiente de invocar.)
- ⏭️ Evaluar **RTS/CTS** y mayor baudrate si se sube el tráfico/TLS.

---

## 11) Licencia
Este repo hereda las licencias de los componentes de terceros utilizados (ESP‑IDF, esp_modem, FirebaseClient, etc.). Revisa cada componente para detalles.

---

## 12) Notas finales
- Los pines y el APN aquí documentados son los que han dado conexión en las pruebas iniciales. Si cambias de carrier/placa, actualiza este README.
- Si encuentras inestabilidad, captura **logs completos** desde `boot:` hasta el fallo e incluye el nivel de log de `esp_modem`/`ppp`.

