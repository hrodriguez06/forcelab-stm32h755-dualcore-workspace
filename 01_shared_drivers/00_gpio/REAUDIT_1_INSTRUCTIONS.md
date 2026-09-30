# Instrucciones para corregir la reauditoría 1 de 00\_gpio

Sep 30, 2026 · auditor · referencia: `README.md` §12.3 (commit `1a635d8`)

La reauditoría revisó lo entregado después de la auditoría A1–A7: `eab46d1`, `8b8919a`, `cdbb914`, `0cd803f`, `6d1ef62` y `1a635d8`.

**El trabajo aprobado:**

- **El fix de A1 en `0cd803f` es correcto.** Enciende el reloj antes de leer `LCKR`, lo que vale para cualquier lectura del silicio. `T16 A1` exige los dos casos, y la mutación M-A1 lo hace fallar.
- **A4 está bien cerrado:** TB da 274 checks en los dos núcleos, y M-A4 da `fail=6` en cada uno.
- **Las cifras de README §9.1 y §9.2 coinciden con las capturas de `0cd803f`.** Comprobé T1, T2, TB, B1, T15 (incluidos `h`, `i` y `j`), T5, T6, T9, T10, T16 A1, los tamaños del `.map`, los warm resets (20/20) y los contadores (0).
- **Las dos corridas son independientes:** las medidas analógicas difieren entre ellas.

Hay cuatro hallazgos nuevos, R1–R4. Ninguno bloquea el merge, pero R1 y R2 cambian código y exigen banco.

Siguen vigentes las reglas de README §14:

- las firmas y los campos existentes quedan congelados, y solo se añade al final;
- `gpio_bm` sigue siendo solo CMSIS;
- cada test lleva un control negativo que imprime su valor;
- las capturas salen de una sola corrida y no se editan;
- antes de mergear: `build_all.ps1 -Clean` 8/8, dos corridas en banco y los warm resets.

No toques este archivo: se borra cuando lo esencial esté en el README (paso 5).

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde `1a635d8` o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`.
2. **Un hallazgo por commit, citando el ID**, por ejemplo `fix(gpio): refuse ReadConfig on an unclocked port (R1)`. Escribe los mensajes desde bash con heredoc: desde PowerShell, el pipe añade un BOM.
3. **No borres ni renombres capturas existentes.** Las que queden sustituidas se marcan como tales en README §9.3.

---

## 1. Orden de trabajo

| Paso | Hallazgo | Por qué en este orden |
| --- | --- | --- |
| 1 | R3 | El host tiene que registrar el parche de cada corrida `-dirty` antes de que se hagan las mutaciones de R1 y R2 |
| 2 | R1 | Cambia el driver y T16 A1 |
| 3 | R2 | Cambia T15; sus mutaciones dependen de R3 |
| 4 | R4 | Solo documentación |
| 5 | — | Banco completo, README y borrado de este archivo (§3) |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 R1: `ReadConfig` devuelve `OK` con valores inventados en un puerto sin reloj

**Hecho medido** (`gpio_selftest_0cd803f.txt`, T16 A1): con el puerto sin reloj en los dos bancos, los registros se leen todo a unos. Por eso `GPIO_BM_ReadConfig` y `GPIO_LL_ReadConfig` devuelven `GPIO_BM_OK` con estos valores:

- `mode = ANALOG`, `otype = OD`, `speed = VERY_HIGH`;
- `pull = 3`, que es un valor reservado: devuelto a `ConfigPin`, lo rechaza con `ERR_PARAM`;
- `af = 15`, `initial_level = 1`.

Es un `OK` que no describe el hardware, contra C12. `IsLocked` responde `true` para todos los pines, y eso ya está documentado (A5).

**Cómo tiene que quedar:**

- **`gpio_bm.h`:**
  - Un estado nuevo **al final** de `GpioBm_Status_t`, después de `GPIO_BM_ERR_NOT_READY`: `GPIO_BM_ERR_NO_CLOCK /*!< port unclocked in both RCC banks: its registers cannot be read */`. Los valores de los estados existentes no cambian.
  - Una función nueva junto a `GPIO_BM_EnablePortClock`: `bool GPIO_BM_IsPortClocked(const GPIO_TypeDef *port);`. Devuelve `true` cuando el bit del puerto está puesto en `RCC_C1->AHB4ENR` o en `RCC_C2->AHB4ENR`, y `false` para un puerto no válido. Ambos son definiciones CMSIS (`stm32h755xx.h`), así que C8 se mantiene. Los dos núcleos pueden leer los dos bancos: T1 lo hace en CM4 y en CM7 en cada corrida (`photo_take`).
  - Si alguna vez se compila para un H7 de un solo núcleo, protege la lectura con `#if defined(DUAL_CORE)` y usa `RCC->AHB4ENR` en la otra rama.
- **`gpio_bm.c`, `GPIO_BM_ReadConfig`:** después de validar `out`, el puerto y el pin, y antes de leer nada del puerto, devuelve `GPIO_BM_ERR_NO_CLOCK` si el puerto no tiene reloj. En ese caso no escribe nada en `*out`.
- **`gpio_ll.c`, `GPIO_LL_ReadConfig`:** lo mismo, usando `GPIO_BM_IsPortClocked`. Así las reglas siguen teniendo una sola copia.
- **`IsLocked` (BM y LL) no cambia de comportamiento.** Su comentario pasa a decir que solo tiene sentido cuando `GPIO_BM_IsPortClocked()` es `true`.
- **`gpio_hal.h`:** `GPIO_HAL_Status()` ya mapea cualquier otro estado a `HAL_ERROR`, así que no cambia. Si algún comentario enumera los estados, añade el nuevo.

**Prueba: caso C de `T16 A1`** (`CM7/App/Src/gpio_selftest.c`, `run_t16_a1`). GPIOK ya está sin reloj en los dos bancos en ese punto, así que va justo después de leer `lckr_off` y antes del caso A:

1. `GPIO_BM_IsPortClocked(GPIOK)` es `false`.
2. Rellena dos `GpioBm_PinConfig_t` y dos `GpioLL_PinConfig_t` con un patrón centinela, por ejemplo `memset(&x, 0xA5, sizeof x)`. Después, `GPIO_BM_ReadConfig(GPIOK, 3U, &x)` y `GPIO_LL_ReadConfig(GPIOK, 3U, &y)` devuelven `GPIO_BM_ERR_NO_CLOCK`, y los dos structs siguen iguales byte a byte a su copia.
3. Control: después del caso A, que enciende el reloj, `IsPortClocked` es `true`, y los dos `ReadConfig` devuelven `OK` con `mode = ANALOG` y `pull = NONE` para PK3.

Imprime una línea `T16 A1 case C ...` con los dos estados, `IsPortClocked` antes y después, y si los centinelas quedaron intactos. El veredicto de `T16 A1` exige `setup && case_a && case_b && case_c`.

**Refuerza además el caso A.** El `MODER` de GPIOK en reset es `0xFFFFFFFF`, justo lo mismo que da un puerto sin reloj. Así que "`MODER` sin cambios" no distingue por sí solo "no se escribió nada" de "no se puede leer". Hoy lo salva la comparación de `LCKR`; hazlo explícito exigiendo `GPIO_BM_IsPortClocked(GPIOK)` después de la llamada del caso A.

**Mutación M-R1** (árbol `-dirty`, con el registro de R3): quita la comprobación de reloj de `GPIO_BM_ReadConfig`. El caso C tiene que fallar. Guarda la captura como `docs/gpio_dev_mutation_readconfig_no_clock_<rev>-dirty.txt`.

**README:**

- §2.1: fila de `GPIO_BM_ERR_NO_CLOCK`, que mapea a `HAL_ERROR`.
- §2.2: `IsPortClocked`, y que `ReadConfig` devuelve `ERR_NO_CLOCK`.
- §10: el punto de A5 dice que `ReadConfig` rechaza un puerto sin reloj y que `IsLocked` depende de `IsPortClocked`.
- §9.1: la fila de `T16 A1` con el caso C.

### 2.2 R2: las carreras de T15 son deterministas para cada imagen

**Hecho medido:** en todas las corridas de una misma imagen, las cifras digitales de T15 salen idénticas:

- `c_raw_both = 315 897` en las cuatro capturas de `780dddd` y `2b9bf21` (el mismo firmware), `322 003` en `8b8919a` y `320 906` en las dos de `0cd803f`;
- en `0cd803f`, `h` = 9 944, `i` = 1 589 corrupciones en 12 821 entradas de ISR, `overlap = 100 033`, y los tiempos en ms;
- en cambio, T5, T6, T9 y T10 sí varían entre corridas.

Después de un reset por SWD, los dos núcleos arrancan siempre con la misma temporización y el bucle no tiene ninguna fuente de variación. Cada fase protegida de T15 prueba, por tanto, **un solo entrelazado** por imagen, y la segunda corrida no añade ninguna muestra independiente de la carrera. README §9 presenta "dos corridas" como si reforzaran también T15.

**Cómo tiene que quedar:**

- **Semilla por corrida, de una fuente no determinista.**
  - Usa el RNG del H755 desde CPU1, en el selftest: `HSI48ON` en `RCC->CR`, espera `HSI48RDY` con un límite, `RCC_AHB2ENR.RNGEN`, y `RNGSEL = HSI48`, que es el valor de reset de `RCC_D2CCIP2R`.
  - Lee un `RNG->DR` con `DRDY` y sin `SECS`/`CECS`, con un límite. Si falla, es un FAIL que imprime el motivo.
  - Escribir `RCC->CR` es de CPU1 por el contrato de arranque; hazlo solo en `GPIO_Selftest_CM7_Run`.
  - Imprime `T15 seed=0x%08lX` antes de la primera fase.
  - Un `#define GST_RACE_SEED` opcional fija la semilla para reproducir una corrida concreta. Si está definido, el log lo indica.
- **Paso de la semilla al CM4:** un campo `race_seed` nuevo en `GstArea_t`, en el grupo T15. CPU1 lo escribe antes de `GST_CMD_RACE_ARM`, cambiándolo en cada fase (por ejemplo, `seed ^ fase`).
- **Jitter en `gst_race_run`:**
  - Un xorshift32 local sembrado con `race_seed ^ (core_index + 1)`.
  - Tras `go`, un retardo inicial de `0..1023` ciclos.
  - En cada iteración, **antes** de la llamada y **fuera** de cualquier sección, un retardo de `0..63` ciclos (`x & 0x3F`) con un bucle de `CYCCNT` local, porque `delay_cycles` es `static` del fichero del CM7.
  - Nada de jitter dentro de la sección: cambiaría lo que mide B1.
- **La fase ISR (`i`, `j`):** basta con el jitter del hilo. Si quieres más, varía también el período inicial de TIM17 con la semilla, dentro de ±5 % de los 20 kHz.

**Criterios de aceptación (dos corridas de la misma imagen):**

- las semillas impresas son distintas;
- `c_raw_both`, `h` e `i` difieren entre corridas y siguen por encima de 0 con `overlap` en los dos núcleos;
- todas las fases protegidas siguen en 0 corrupciones, con `locked_calls` exacto;
- el margen de B1 sigue siendo ≥ 10×.

Si una fase protegida falla, la semilla impresa, puesta en `GST_RACE_SEED`, tiene que reproducir el fallo.

**Mutación M-R2:** la de `780dddd`, `gpio_ll` sin su sección, con jitter. T15 `d` y `e` tienen que dar corrupciones distintas en dos corridas, y ambas por encima de 0.

**README:**

- §9: cambia la frase de las dos corridas. Lo que era determinista antes de R2 eran los resultados digitales.
- §9.1: fila de T15 con la semilla y los rangos de las dos corridas, no un solo valor.
- §9.3: las capturas de T15 anteriores a R2 quedan como "un entrelazado por imagen".

### 2.3 R3: las capturas de mutación no registran su parche

`gpio_dev_mutation_*_0cd803f-dirty.txt` solo dicen `Rev 0cd803f-dirty`. Qué se mutó se sabe por el nombre del fichero y por el mensaje de `6d1ef62`, no por la captura. Con eso no se puede reproducir ni comprobar desde la propia evidencia (C9).

**Cómo tiene que quedar:** en `tools/gpio_hw_check.py`, cuando la revisión es `-dirty` y se pasó `--allow-dirty`, la cabecera de la captura incluye, antes de abrir el AD2:

- una línea `[host] dirty diff sha256=<hash> lines=<n>`;
- el resultado completo de `git diff --no-color HEAD` de los ficheros versionados, cada línea con el prefijo `[host] diff| `;
- nada de `git add`, `stash` ni otra operación que modifique el árbol.

En una revisión limpia, una sola línea `[host] tree clean`. Usa solo la biblioteca estándar (`subprocess`, `hashlib`), no paquetes de terceros.

**Prueba:**

- una corrida `-dirty` real (la de M-R1 sirve) muestra su parche en la captura;
- una corrida limpia muestra `tree clean`;
- si el script tiene pruebas unitarias, añade un caso con `subprocess` simulado.

**Las capturas de `0cd803f-dirty` no se editan.** En README §9.3 añade, en su fila, qué parche describía cada una, citando `6d1ef62`, y aclara que la captura no lo registra.

### 2.4 R4: el hecho de silicio medido no está en §6

El resultado de `T16 A1` es un hecho del silicio que RM0399 no recoge: en Rev V, un puerto GPIO sin reloj en los dos bancos se lee `0xFFFFFFFF`. Hoy solo figura en §9.1, §10 y §12.1.

**Cómo tiene que quedar:** una fila nueva en la tabla de README §6, con esta forma:

| Regla | Mecanismo | Test |
| --- | --- | --- |
| **Medido (Rev V):** un puerto GPIO sin reloj en los dos bancos de RCC se lee todo a unos (`LCKR = 0xFFFFFFFF`, real `0x00010023`) | Toda llamada de configuración o de lock enciende el reloj antes de su primera lectura; `ReadConfig` rechaza un puerto sin reloj (R1) | T16 A1 |

No afirmes nada sobre las **escrituras** en un puerto sin reloj: ningún test lo ha medido.

---

## 3. Entrega

1. `.\tools\build_all.ps1 -Clean`: 8/8. `selftest-gpio` de `prj_01` y `prj_04` compila. Los tres `.c` del driver, con `-Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual`, dan 0 warnings en CM7 y CM4.
2. Dos corridas completas de `gpio_hw_check.py` desde un worktree limpio, en la misma revisión y con semillas distintas. Las dos tienen que dar `FW VERDICT=PASS` y `HOST VERDICT=PASS`.
3. Warm resets de `debug` y de `release` (20/20 cada uno) y contadores de fallos del lock en 0.
4. Las mutaciones M-R1 y M-R2, con el parche en la captura (R3).
5. README:
   - §12.3: cada R pasa a su estado final con su prueba.
   - §9.1, §9.3 y §15: actualizados.
   - Cabecera: la línea de estado cita la nueva revisión.
   - Cuando todo lo necesario para un usuario esté en el README, borra este archivo en un commit propio, como se hizo en `cc1ee88`.

**En la siguiente reauditoría comprobaré:**

- con el diff contra `1a635d8`, que los `.h` solo añaden líneas o cambian comentarios;
- que `GPIO_BM_ERR_NO_CLOCK` va al final del enum;
- el caso C y la mutación M-R1 en las capturas;
- que las semillas de las dos corridas difieren y que las cifras de T15 cambian con ellas;
- que las capturas `-dirty` llevan su parche;
- y otra vez cada cifra del README contra su captura.
