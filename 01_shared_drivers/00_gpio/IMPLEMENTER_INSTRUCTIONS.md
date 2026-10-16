# Instrucciones para el implementador de `00_gpio` F2: EXTI 0–15 y celda de compensación

Oct 15, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 3) de esta carpeta:
- las funciones EXTI y de la celda de compensación en `gpio_bm`;
- sus pruebas en el `selftest-gpio` de `prj_01` y en su herramienta de banco;
- la herramienta sin placa `tools/gpio_f2_checks.py`.

Las firmas existentes de `gpio_bm.h` no cambian: solo se añaden. Otra persona auditará tu fase y ejecutará en la placa todo lo que necesita la placa.

Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de la sección 2.2, que son absolutas.

---

## 1. Tu papel

**Tú implementas y pruebas sin placa. No auditas.**

- El estado final de tu entrega es "entregado para auditoría", nunca "auditado", "aprobado" ni "cerrado".
- No crees, edites ni borres `01_shared_drivers/00_gpio/AUDIT.md`. Si no estás de acuerdo con una observación, argumenta en tu informe.
- Un hallazgo corregido se declara "corregido en `<hash>`, pendiente de verificación".
- No toques los README hasta que `AUDIT.md` diga que la fase pasa.
- **No afirmes nada que una captura commiteada no demuestre.** Todo lo que necesita la placa o el AD2 queda NO VERIFICADO hasta la ejecución del auditor.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Propósito |
| --- | --- |
| `01_shared_drivers/00_gpio/gpio_bm.h`, `gpio_bm.c` | H1: solo añadir (plan, Diseño 1 y 2) |
| `03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c`, `CM4/App/Src/gpio_selftest.c`, `Common/Inc/gpio_selftest.h`, `Common/Src/gpio_selftest_common.c` | H2 |
| `03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py` | H3 |
| `tools/gpio_f2_checks.py` (nuevo) | H4 |
| `03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_*` | Tus capturas sin placa |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | H5 |
| `01_shared_drivers/00_gpio/README.md` | Solo en el cierre, cuando el auditor lo autorice |

Nada más (`git diff 3df160a --stat`).

### 2.2 Prohibido

| Archivo o acción | Razón |
| --- | --- |
| Cambiar o borrar cualquier declaración existente de `gpio_bm.h`, `gpio_ll.h` o `gpio_hal.h` | API congelada (C2). Si falta algo, para y pregunta |
| `tools/ad2.py`, las guardas S3, S4, S5, S11 y sus listas de DIO permitidas | Seguridad eléctrica del banco (Decisión 2) |
| Hacer que `bench_claim()` acepte PE0–PE4 o PE7, o hacer salida del AD2 DIO9 o DIO11 | S1–S4: contención sin resistencias en serie |
| `SystemClock_Config`, `HAL_PWREx_ConfigSupply`, option bytes, `stm32h7xx_it.c` | Fuera de alcance; T17 toca `CSION` solo dentro de la prueba |
| La línea EXTI 15 y H5c de `prj_04` | Las usa `10_hsem` |
| Programar, resetear o leer la placa; abrir el AD2 (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`, `gpio_hw_check.py` contra hardware, `run_warm_resets.py`) | **No tienes acceso a la placa ni al AD2.** El host se prueba con el `_MockDwf` del propio `gpio_hw_check.py` |
| Retocar capturas commiteadas; mezclar codificaciones | Integridad de la evidencia |
| Merge, borrar ramas o documentos de proceso | Los hace el responsable |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/gpio-f2`**, desde `main` en `3df160a`. El plan y estas instrucciones ya están en ella.
- Un commit por objetivo, en inglés e imperativo, con el porqué en el cuerpo. Por ejemplo:
  - `feat(gpio): add EXTI 0-15 and compensation cell to gpio_bm (H1)`
  - `test(prj01): add F2 EXTI and compensation-cell steps to selftest-gpio (H2)`
  - `test(prj01): drive and measure the F2 steps in gpio_hw_check.py (H3)`
  - `feat(tools): add gpio_f2_checks.py (H4)`
  - `test(gpio): record T1 build and T18 runs (no board)`
  - `docs(gpio): add F2 implementation report`
- En PowerShell, cuidado con el BOM: escribe los mensajes en un archivo UTF-8 sin BOM y usa `git commit -F`.

### 3.2 Decisiones

El responsable aprobó las cuatro el 2026-10-15: 1A, 2A, 3A y 4. Aplícalas.

### 3.3 Entorno

- `.\tools\env.ps1` en cada consola nueva; la matriz con `.\tools\build_all.ps1 -Clean`.
- `py`, nunca `python`; solo la biblioteca estándar.
- El control `3df160a` se compila en un worktree aparte (`git worktree add ../stm32h755_control_3df160a 3df160a`). Tu árbol queda limpio. Si enlazas el submódulo de Cube, dilo en el informe.

### 3.4 El driver (H1)

Sigue el Diseño 1 y 2 del plan al pie de la letra. En especial:
- **Núcleo:** `CORE_CM4` elige los registros de CPU2.
- **Validación (C6):** todo se valida antes de escribir.
- **Sección (C10):** todo lo compartido se escribe en la sección (`RTSR1`, `FTSR1`, `SWIER1`, `EXTICR`, `CCCSR`), y las comprobaciones del otro núcleo van dentro de la misma sección.
- **Orden:** pending borrado antes de desenmascarar.
- **`ExtiTakePending`:** sin sección y con `__DSB()`.
- **`EnableCompensationCell`:**
  - sin CSI, `ERR_NOT_READY` sin escribir;
  - la espera, acotada por `DWT->CYCCNT`;
  - nunca escribe `RCC_CR`, `HSLV`, `CS` ni `CCCR`.
- **Restricciones:** sin handlers (C14); sin RAM nueva (C7); solo CMSIS.
- **Comentarios:** en inglés, con la cita de RM0399 de cada regla.

### 3.5 Las pruebas del selftest (H2)

- Las ISR de prueba viven en los `gpio_selftest.c`, bajo la opción. Primera instrucción: el `BSRR` del pin de respuesta (CM7 → PE5, CM4 → PE6). Contadores privados por núcleo.
- Los pasos nuevos van en `k_steps[]` con su espejo exacto en `STEPS` del host. `bench_claim()` permite PE5/PE6 como salida **solo** en esos pasos.
- **Comandos nuevos de CM4:** se añaden al final de `GstCmd_t`, y sus campos al final de `GstArea_t`, que no puede pasar de 4 KB. Ningún campo existente se mueve.
- **Cada prueba deja todo como estaba:** `ExtiDisable`, NVIC apagado y sin pending, pines de respuesta a 0 y en entrada. T17 restaura `CSION` y `CCCSR` e imprime los dos.
- **Formato:** las líneas, con el de F1: `[gpio] T11 ... -> PASS|FAIL`. El veredicto global sigue siendo `FW VERDICT=`.
- **Orden:** T11–T14 y T17 entre T10 y T16.

### 3.6 El host (H3)

- **Pulsos:** escrituras estáticas en DIO8/DIO10 por `ad2.set_oe()` y `digital_io_write`, al menos 1 ms entre flancos, desde 0 y de vuelta a 0. OE vuelve a 0 al final de cada paso.
- **LA:** captura DIO8–DIO11 a 100 MS/s para la latencia (T13), con muestras suficientes; si no caben, en tandas, y lo dice la captura.
- **Checks:** cada uno es un `[host] CHECK <nombre>: PASS|FAIL` que entra en `HOST VERDICT=`:
  - flancos enviados según el LA;
  - cuentas de la ISR leídas de la consola;
  - respuestas según el LA;
  - mín, p50 y máx de latencia.
- **Prueba sin placa:** extiende la del `_MockDwf` para los pasos nuevos, incluido un caso en el que el firmware cuenta de menos y el host lo detecta.

### 3.7 La herramienta sin placa (H4, T18 y T1 de compilación)

`tools/gpio_f2_checks.py`:
- Recibe `--test {t1,t18,all}` y `--output`.
- Comprueba el árbol limpio y que el control sea de `3df160a`.
- Implementa todas las comprobaciones de T18 del plan, cada una con su control negativo:
  - una copia temporal de la cabecera con una firma cambiada;
  - un `.map` con un símbolo `GPIO_BM_Exti*` de más en producción.

  Los controles negativos se generan dentro de la ejecución, sin tocar el árbol.

### 3.8 Seguridad del banco

No usas la placa. Lo que escribas tiene que respetar S1–S11 y G-S1–G-S5 cuando lo ejecute el auditor.

---

## 4. Orden de trabajo recomendado

1. **H1** y su compilación en todos los presets.
2. **H4:** T18 contra el control, con sus controles negativos fallando.
3. **H2, H3 y la prueba del host con el mock.**
4. **T1 de compilación y T18:** capturas commiteadas.
5. **H5:** el informe, con:
   - los comandos exactos del banco para el auditor (flasheo, `gpio_hw_check.py` dos veces y `run_warm_resets.py`);
   - lo que el responsable tiene que preparar: J2/J3 fuera, el USB de la F334 desconectado y el AD2 con GND primero.
6. **Entrega.** Commit con el árbol limpio. La **última línea** de `IMPLEMENTATION_REPORT.md` es exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`.

---

## 5. El informe (`IMPLEMENTATION_REPORT.md`)

1. Resumen: commits, cambios y estado ("entregado para auditoría").
2. El driver: cada regla del Diseño 2 con `archivo:línea`.
3. Resultados sin placa (T1 de compilación, T18 y la prueba del host con el mock): veredicto, control negativo y captura.
4. Las pruebas de placa (T11–T14, T17, banco completo): qué hace cada paso y qué imprime. Todas NO VERIFICADAS.
5. Los comandos y la preparación del banco para el auditor.
6. Desviaciones respecto al plan, con su motivo.

---

## 6. Checklist de entrega

- [ ] `git status --porcelain` vacío.
- [ ] `build_all.ps1 -Clean` 8/8 y `selftest-gpio` de `prj_01` y `prj_04` sin avisos.
- [ ] `git diff 3df160a -- '01_shared_drivers/00_gpio/*.h'` solo añade.
- [ ] Ningún handler en `00_gpio/`; `.data = 0` y `.bss = 8 B`.
- [ ] `ad2.py` sin cambios; PE0–PE4 y PE7 siguen rechazados en `bench_claim()`.
- [ ] Los controles negativos de T18 fallan como deben, y la prueba del host con el mock detecta la cuenta de menos.
- [ ] El informe no da por hecha ninguna prueba de placa y su última línea es la marca de entrega.
