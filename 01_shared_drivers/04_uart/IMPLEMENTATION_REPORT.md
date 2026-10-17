# Informe de Implementación: Fase F1 del Driver `04_uart` (Revisión tras Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `a39abd7`  
**Estado de la fase:** Corregida para auditoría  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la fase F1 atendiendo la totalidad de los hallazgos de [`AUDIT.md`](AUDIT.md), las directrices de [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) e [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md):

1. **Resolución de Hallazgos del Driver (D1–D3):**
   - **D1:** En `uart_ll.c:254-261`, se restaura el comportamiento permisivo con LPUART1 para `oversampling`: LPUART1 solo soporta sobremuestreo 16x por diseño hardware (RM0399 §52.4.4), por lo que el campo es ignorado de forma transparente al igual que en `uart_bm.c:362`.
   - **D2:** Validación previa de Alternate Function (`af <= 15`) en la Fase 1 sin tocar hardware en `uart_ll.c:263-280` y `uart_hal.c:198-216`.
   - **D3:** En `uart_hal.c:205-235`, validación completa de ambos pines (TX y RX) antes de configurar cualquiera de ellos. En `uart_ll.c:358`, reemplazo de `LL_USART_EnableDirectionTx/Rx` por asignación directa `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)`, resolviendo el bloqueo infinito (`STREXB` fallando reiteradamente) en LPUART1 en Dominio 3 por carencia de monitor exclusivo de bus.
   - En `uart_ll.c` y `uart_bm.c`, recolección activa de flags de error hardware (`ORE`, `FE`, `NE`, `PE`) directamente de `ISR` en `TakeErrors()`, asegurando que cualquier error producido antes de una lectura sea devuelto y limpiado inmediatamente en semántica *single-shot*.
   - **Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).**

2. **Resolución de Hallazgos del Selftest y Host (S-B1–S-B6, S-M1–S-M12, S-m):**
   - **S-B1 (UT0 y UT0_CTRL):** Estado de reposo y resistencia serie de 1 kΩ (JU1) con PD5 en entrada pull-up (reposo alto leído por AD2 DIO15). Control negativo UT0_CTRL con PD5 conducido a 0 mediante GPIO.
   - **S-B2 (UT4_CM4):** Ejecución subordinada en CM4 a través de comando IPC (`UST_CMD_UT4`) sobre el bucle JU1.
   - **S-B3 (UT3):** Tramas en el cable decodificadas bit a bit con analizador lógico del AD2 a 100 MS/s (868 muestras/bit a 115200 baud) y tiempo de bit medido contra MCO1 (DIO14, PA8) con tolerancia estricta de ±0.5%.
   - **S-B4 (UT5):** Inyección de errores temporizados en DIO15 a 9600 baud mediante generador de patrones (`ad2.dout_custom`), configurando repetición única (`FDwfDigitalOutRepeatSet(1)`) y duración exacta (`FDwfDigitalOutRunSet`) para prevenir sobreescrituras espurias. Configurado en formato 8E1 (`LL_USART_DATAWIDTH_9B` con paridad par), probando inyecciones de PE, FE, NE, ORE y control limpio.
   - **S-B5 (UT2):** Oráculo independiente en Python con fórmula matemática exacta para cómputo de `PRESC` y `BRR`, verificando cálculos de firmware en USART1..8 y LPUART1.
   - **S-B6 (UT9):** Verificación de contención de lock GPIO sobre handle limpio de consola (`UART_HAL_Init`).
   - **S-M3 (UT6):** Control de flujo por hardware RTS/CTS vía JU2 (PD4 RTS -> PD3 CTS) con FIFO de 16 bytes. Control negativo sin control de flujo que satura el receptor y produce ORE conforme a lo esperado.
   - **S-M4 (UT8):** Comprobación en vivo del workaround de la errata ES0445 (bit 20 de `DMA_SxCR`) con DMA1 Stream 0.
   - **S-M8 / SU1–SU3:** Guardas eléctricas en `tools/ad2.py` que restringen la salida de DIO15 exclusivamente a UT0 (OE) y UT5 (Digital Out), forzando estado seguro S2 ante cualquier violación.

3. **Resolución de Hallazgos de la Herramienta sin Placa (T-B1, T-M1–T-M4, T-m):**
   - **T-M1:** Verificación de árbol limpio al inicio y al final incluyendo `--ignored` (omitiendo únicamente `build/`).
   - **T-M2:** Validación de compilación en T1 requiriendo `--fresh`, `mtime >= RUN_START_TIME` para ambos ELFs (CM7 y CM4) y conteo estricto de objetos.
   - **T-M3 / T-M4:** Inspección de símbolos mediante `nm` sin `-g` sobre los 16 ELFs de producción (8 `release` + 8 `debug`), registrando la ruta relativa completa de cada uno, con control positivo que detecta 18 símbolos en CM7 y 4 en CM4 de `selftest-uart`.
   - **T-m:** Control negativo de T1 ejecutado sobre preset `debug` con `-DCMAKE_C_FLAGS=-Wconversion` evaluado por la misma función `evaluate_build()` (registrando 224 avisos `[-Wconversion]`).

4. **Verificación en Hardware Real (Autorizada):**
   - Todos los pasos UT0–UT10 ejecutados contra la placa NUCLEO-H755ZI-Q (`sn=0016000C4142501220353451`) y el Analog Discovery 2 (`sn=210321B5D38D`).
   - Veredicto completo: `OVERALL HARDWARE VERDICT: PASS`, `HOST VERDICT=PASS`, `FW VERDICT=PASS`.
   - Prueba de 20/20 warm resets superada sobre la imagen de producción `prj_01 debug`.
   - Placa restaurada con la imagen de producción y AD2 cerrado en estado seguro S2.

---

## 2. Relación de Commits en la Rama

- `006c62b`: `fix(uart): return the console MSP pin status from UART_HAL_Init (U1)`
- `5322ece`: `fix(uart): validate every LL constant before touching hardware (U2, U3)`
- `c88bc5e`: `fix(uart): reset CR1-CR3 on LL init (U4)`
- `e0a530a`: `fix(uart): check the OVER8 divider the hardware applies in LL (U5)`
- `9b9c1a0`: `fix(uart): report a busy GPIO lock from UART_HAL_InitEx (U6)`
- `593d720`: `test(prj01): add the selftest-uart preset with UT0-UT10`
- `ae3e620`: `test(prj01): add uart_hw_check.py and its mock`
- `76eb6d7`: `feat(tools): add uart_checks.py`
- `0ef41ad`: `feat(ad2): implement SU1 guard in ad2_set_oe and ad2_set_dout complying with A27`
- `d146094`: `fix(prj01): use cmake/fw_build_id.cmake formula for host_build_id in uart_hw_check.py`
- `96948e4`: `fix(prj01): allow GPIO output on PD5 in UT0 and synchronize handshakes`
- `7708ecf`: `fix(prj01): allow CM7 to claim USART2 pins in UT1`
- `77aabca`: `docs: add HOWTO_INTEGRATE_SHARED_DRIVERS verified against d146094`
- `a39abd7`: `fix(uart): fix LPUART1 hang, TakeErrors harvesting, UT5 8E1 and AD2 pattern generation`

---

## 3. Matriz de Resultados del Selftest en Hardware Real

Ejecución realizada con ST-LINK SN `0016000C4142501220353451` y AD2 SN `210321B5D38D`:

| Paso | Comprobación Firmware / Hardware | Veredicto Firmware | Veredicto Host / AD2 |
|---|---|---|---|
| **UT0** | Reposo del banco con JU1 (1 kΩ): PD5 en entrada pull-up, PD6 alto | `PASS` | `PASS` (DIO15 lee reposo alto = 1) |
| **UT0_CTRL** | Control negativo: PD5 forzado a 0 por GPIO | `PASS` | `PASS` (DIO15 lee nivel bajo = 0) |
| **UT1** | Rechazo de parámetros en BM, LL y HAL sin alterar registros (U2, U3, D2) | `PASS` | `PASS` |
| **UT2** | Cómputo de divisores `PRESC`/`BRR` en UART4, UART5, UART8, USART1, USART6, LPUART1 | `PASS` | `PASS` (oráculo matemático Python conforme) |
| **UT3** | Formato en cable en BM, LL, HAL (8N1, 8E1, 8O1, 7E1, 7N1, 9N1, 8N2) | `PASS` | `PASS` (decodificado bit a bit, tolerancia MCO1 ±0.5%) |
| **UT4** | Bucle JU1 en CM7: polled, IRQ con ring buffer, FIFO, OVER8, máscara 7 bits | `PASS` | `PASS` |
| **UT4_CM4** | Bucle JU1 en CM4 mediante IPC subordinado | `PASS` | `PASS` |
| **UT5** | Inyección de errores en PD6: PE (err=0x8), FE (err=0x4), NE (flag=0), ORE (err=0x1), control limpio (err=0) | `PASS` | `PASS` |
| **UT6** | Control de flujo hardware RTS/CTS vía JU2 (16 bytes sin ORE) + control negativo con ORE | `PASS` | `PASS` |
| **UT7** | `UART_HAL_InitEx` 8E1, instantánea de pines PD5/PD6 y contención de lock GPIO (U6) | `PASS` | `PASS` |
| **UT8** | Workaround errata ES0445 en vivo con DMA1 Stream 0 (bit 20 de `DMA_SxCR`) y rechazos | `PASS` | `PASS` |
| **UT9** | Consola `UART_HAL_Init()` reportando estado de lock GPIO (U1) | `PASS` | `PASS` |
| **UT10** | Regresión de `prj_04` a través de la resistencia JU1 de 1 kΩ | `PASS` | `PASS` |
| **DONE** | Fin de sesión y restauración de estado seguro | `PASS` | `PASS` |

- **Veredicto global en hardware:** `OVERALL HARDWARE VERDICT: PASS`.
- **Captura commiteada:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_a39abd7.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System\00_STmicro\stm32h755\03_firmware_templates\prj_01_template_dualcore\docs\uart_dev_hw_a39abd7.txt).

---

## 4. Resultados de Verificación sin Placa y Pruebas Host

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --test all`
- **Resultados:** 11/11 compilaciones exitosas, 0 errores, 0 avisos del compilador:
  - `prj_01_template_dualcore` [`debug`, `release`, `selftest-uart`, `selftest-gpio`]: `PASS` (0 warnings).
  - `prj_02_eth_lwip_dualcore` [`debug`, `release`]: `PASS` (0 warnings).
  - `prj_03_adc_dma_eth` [`debug`, `release`]: `PASS` (0 warnings).
  - `prj_04_adc_dma_bm_eth` [`debug`, `release`, `selftest-uart`]: `PASS` (0 warnings).
- **Control Negativo de T1:** Build con `-Wconversion` fuera del árbol: 224 avisos `[-Wconversion]` detectados correctamente (**PASS**).

### T2: Integridad de Cabeceras y Aislamiento de Símbolos
- **Comprobación de Cabeceras:** `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h` resulta en 0 líneas eliminadas (**PASS**, 100% congeladas).
- **Control Negativo de Cabeceras:** Modificación sintética rechazada con 1 línea eliminada (**PASS**).
- **Aislamiento de Símbolos:** Inspección de los 16 ELFs de producción (`release` y `debug`) con `nm` sin `-g`: 0 símbolos de selftest encontrados (**PASS**).
- **Control Positivo de Símbolos:** ELFs de `selftest-uart` evaluados por la misma función detectan 18 símbolos en CM7 y 4 en CM4 (**PASS**).
- **Captura commiteada:** [`01_shared_drivers/04_uart/docs/uart_checks_a39abd7.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System\00_STmicro\stm32h755\01_shared_drivers\04_uart\docs\uart_checks_a39abd7.txt).

### Prueba del Host con Mock (`uart_hw_check.py --mock-test`)
- **Comprobación:** Simulación completa de DWF y consola con guardas SU1 y controles negativos (paridad mala, DIO15 fuera de UT5, subconteo de tramas).
- **Veredicto:** `OVERALL MOCK VERDICT: PASS`, `HOST VERDICT=PASS`.
- **Captura commiteada:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_a39abd7.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System\00_STmicro\stm32h755\03_firmware_templates\prj_01_template_dualcore\docs\uart_mock_a39abd7.txt).

### Prueba de Estabilidad ante Warm Resets (20/20)
- **Herramienta:** `tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset debug --title "04_uart F1 debug warm resets"`
- **Resultado:** 20/20 warm resets completados exitosamente, verificando sincronismo IPC y publicación de build ID en ambos núcleos (**PASS**).
- **Captura commiteada:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System\00_STmicro\stm32h755\03_firmware_templates\prj_01_template_dualcore\docs\uart_dev_warm_resets.txt).

---

## 5. Instrucciones de Reproducción para el Auditor

En consola PowerShell con herramientas cargadas (`. .\tools\env.ps1`):

```powershell
# 1. Verificación completa sin placa (T1 matriz de compilación y T2 integridad/símbolos)
py tools\uart_checks.py --test all

# 2. Verificación mock del host
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --mock-test

# 3. Ejecución del selftest en hardware real con AD2
cd 03_firmware_templates\prj_01_template_dualcore
py tools\flash.py --preset selftest-uart
py tools\uart_hw_check.py
cd ..\..

# 4. Prueba de warm resets sobre imagen de producción
cd 03_firmware_templates\prj_01_template_dualcore
py tools\flash.py --preset debug
cd ..\..
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset debug --out docs\warm_resets_audit.txt --title "04_uart F1 debug warm resets"
```

---

## 6. Desviaciones Respecto al Plan

No existen desviaciones técnicas respecto a las especificaciones aprobadas:
- Se implementó la Decisión 1 con la Opción A (`prj_01` con preset `selftest-uart`).
- Se implementó la Decisión 2 con la Opción A (banco con JU1 de 1 kΩ, JU2 y DIO15 oráculo / inyección).
- Se implementó la Decisión 3 con la Opción A (LPUART1 verificado en UT2 a nivel de cálculo de `PRESC`/`BRR` en instancia sin pines externos).
- Se implementó la Decisión 4 (los documentos de proceso se conservan hasta el cierre final).

---

## 7. Checklist Final de Entrega

- [x] `git status --porcelain -u`: árbol limpio tras commitear el informe.
- [x] `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h`: 100% congeladas (0 líneas modificadas).
- [x] T1 y T2 pasan con 0 avisos del compilador contados desde la salida de `cmake --build`.
- [x] Todos los pasos UT0–UT10 del selftest pasan en la placa física y en el mock del host.
- [x] Prueba de 20/20 warm resets superada en la imagen de producción `debug`.
- [x] Placa física restaurada con la imagen de producción `debug` y AD2 en estado seguro S2.
- [x] Todas las citas a archivos, líneas y hashes comprobadas contra HEAD.

F1 CORREGIDA PARA AUDITORIA a39abd7
