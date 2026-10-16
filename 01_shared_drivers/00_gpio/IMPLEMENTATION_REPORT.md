# Informe de Implementación: Fase 00_gpio F2 (Corrección de Auditoría)

**Fecha:** 2026-10-16  
**Rama:** `driver/gpio-f2`  
**Estado:** F1 CORREGIDA PARA AUDITORÍA  
**Último commit de código o herramienta:** `bc7e64d`

---

## 1. Resumen de la entrega corregida

Se han corregido la totalidad de los hallazgos señalados en la auditoría inicial de la fase 00_gpio F2 (`AUDIT.md`), cubriendo los 4 bloqueantes (B1–B4), los 6 mayores (M1–M6) y los 7 menores (m1–m7), cumpliendo estrictamente con el plan de implementación (`IMPLEMENTATION_PLAN.md`, revisión 2) y las instrucciones del implementador (`IMPLEMENTER_INSTRUCTIONS.md`).

Todas las comprobaciones sin placa (T1 de compilación, T18 de regresión contra el control `3df160a`, y la prueba del host con el mock DWF) fueron repetidas y superadas con veredicto favorable (`PASS`), respaldadas íntegramente por las capturas commiteadas en `03_firmware_templates/prj_01_template_dualcore/docs/`.

### Historial completo de commits en la rama `driver/gpio-f2`
1. `628eea6` — `feat(gpio): add EXTI 0-15 and compensation cell to gpio_bm (H1)`
2. `25fbe4f` — `feat(tools): add gpio_f2_checks.py (H4)`
3. `0a025c9` — `test(prj01): add F2 EXTI and compensation-cell steps to selftest-gpio (H2)`
4. `ed4f31c` — `test(prj01): drive and measure the F2 steps in gpio_hw_check.py (H3)`
5. `55e9119` — `test(gpio): record T1 build, T18, and mock runs (no board)`
6. `cbd68a9` — `docs(gpio): add F2 implementation report`
7. `3497df9` — `docs(gpio): point the F2 implementer instructions to plan revision 2`
8. `31c5187` — `fix(gpio): correct EXTI read-back, hold time, and headers in gpio_bm (H1)`
9. `7fb71ca` — `fix(prj01): correct selftest negative controls, authorization, and compensation cell (H2)`
10. `493a152` — `fix(prj01): add LA edge counting, T13 latency measurement, and mock fixes (H3)`
11. `bc7e64d` — `fix(tools): overhaul gpio_f2_checks.py with pure functions and clean checks (H4)`
12. `25add1f` — `test(gpio): record T1 build, T18 regression, and mock test captures for bc7e64d`

---

## 2. Matriz de resolución de hallazgos de auditoría

| ID | Clase | Hallazgo de auditoría | Acción correctiva aplicada y verificación |
| --- | --- | --- | --- |
| **B1** | Bloqueante | Cadena de T17 discordante entre firmware (`without CSI (refused) and with CSI (ready)`), host (`ready with CSI and refused without`) y mock. | Cadena unificada exactamente a `"T17 compensation cell without CSI (refused) and with CSI (ready)"` en [gpio_selftest.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1995), [gpio_hw_check.py](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1126) y en la simulación del mock. |
| **B2** | Bloqueante | Espera de `READY` dentro de la sección crítica en `GPIO_BM_EnableCompensationCell` superaba los 60 µs y provocaba fallo en silicio. | Rediseño conforme a Regla 9 (Plan rev 2): dentro de la sección se escribe `CCCSR.EN = 1` y se verifica lectura; se sale inmediatamente de la sección (`GPIO_BM_SectionExit`). La espera de `READY` se realiza fuera de la sección mediante `DWT->CYCCNT` con límite de 4 000 000 ciclos (~10 ms). En T17 se mide el tiempo de retención de la sección con un shim, verificando retención < 60 µs. |
| **B3** | Bloqueante | T11 y T12 no medían pulsos ni respuestas en el Analizador Lógico (LA); T13 usaba 20 flancos propios en vez de los de T11. | Se implementó `DigitalInHelper` en `gpio_hw_check.py` con soporte de flancos de subida y bajada. En T11, el LA captura y cuenta cada flanco enviado (200 en RISING/FALLING, 400 en BOTH) y cada respuesta en DIO9/DIO11 a 100 MS/s. Durante `T11_CM7_RISING` y `T11_CM4_RISING` se recolectan 200 mediciones de latencia que T13 reporta directamente. En T12, el LA mide DIO8, DIO9 y DIO11 en los tres subtests. |
| **B4** | Bloqueante | Controles negativos constantes o literales en `gpio_f2_checks.py`; builds sin `-Clean` ni `--clean-first`. | Se implementaron funciones puras (`verify_header_diff`, `verify_no_driver_handlers`, `verify_no_production_exti_handlers`, `verify_driver_sizes`, `compare_symbols`) invocadas con entradas reales y con entradas mutadas sintéticas temporales. Se enforced `-Clean` en `build_all.ps1` y `--clean-first` en los selftests, verificando objetos compilados > 0 y 0 advertencias. |
| **M1** | Mayor | El driver no leía de vuelta lo que escribía en `ExtiConfig`, `ExtiDisable` ni `enable_syscfg_clock`. | Se añadió lectura de vuelta dentro de la sección crítica en `GPIO_BM_ExtiConfig` (`EXTICR`, `RTSR1`, `FTSR1`, `IMR1`/`EMR1`), en `GPIO_BM_ExtiDisable` y en `enable_syscfg_clock` (`RCC_APB4ENR_SYSCFGEN`), retornando `GPIO_BM_ERR_VERIFY` en caso de discrepancia. |
| **M2** | Mayor | `gpio_f2_checks.py` no fallaba con árbol sucio; tamaños de `.obj` en vez de `.map`; búsqueda incompleta de ISR en producción; omisión de cabeceras; submódulo Cube no documentado. | Árbol sucio ahora aborta la ejecución (salvo `--allow-dirty` explícito en desarrollo); el control `3df160a` verifica limpieza con `--ignore-submodules` y documenta la unión (junction) a `STM32CubeH7`; los tamaños `.data = 0 B` y `.bss = 8 B` se extraen directamente del mapa de enlace `.map`; se verifica ausencia de símbolos fuertes `EXTI*_IRQHandler` (`T`/`t`) en las 16 imágenes de producción; se inspeccionan todas las cabeceras `*.h`; la captura incluye `argv`, `HEAD`, opciones y salida de compilación. |
| **M3** | Mayor | CM4 configuraba PE6 como salida sin `bench_claim()`, y tras aborto CM7 seguía enviando `F2_CONFIG`, rompiendo S4. | CM7 valida `!s_bench_abort && bench_claim(GPIOE, 1U << PE_SPARE6)` antes de enviar `F2_CONFIG` con la bandera `GST_F2_AUTH_PE6`; CM4 rechaza la configuración de salida si no recibe dicha autorización; comentario de CM7 corregido. |
| **M4** | Mayor | Controles negativos no aislaban lo probado (NVIC apagado en EVENT; NVIC apagado en CM7 T13; PE6 vuelto a entrada en CM4 T13). | NVIC permanece habilitado y pines de respuesta en salida (`PE5`/`PE6`) durante los controles negativos de T11 EVENT y T13 CM7/CM4; se aísla exclusivamente el desenmascaramiento EXTI. |
| **M5** | Mayor | T14 carecía de oráculos completos (lectura directa de `PR1`, restauración a PE, `BOTH` en regla 6, retorno de `ExtiTakePending`). | T14 lee `PR1` directamente tras el pulso (1) y tras `ExtiConfig` (0); restaura la ruta a `PE` tras probar `GPIOD`; evalúa trigger `GPIO_BM_EXTI_BOTH` en regla 6 verificando `RTSR1` y `FTSR1`; comprueba que `ExtiTakePending` devuelva el bit 0. El host valida cada subprueba como un `[host] CHECK` independiente. |
| **M6** | Mayor | Afirmaciones en el informe previo sin respaldo en capturas. | Todas las afirmaciones del presente informe provienen de las capturas commiteadas `gpio_f2_checks_bc7e64d.txt` y `gpio_f2_mock_bc7e64d.txt`. Los resultados en silicio de la corrida de diagnóstico se citan como medidos por el auditor. |
| **m1** | Menor | `SYSCFGEN` se escribe antes de la sección crítica. | Documentado explícitamente en el encabezado de [gpio_bm.h](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L329-L334). |
| **m2** | Menor | Citas de RM0399 erróneas en cabecera y código. | Citas corregidas: `RTSR1` §21.6.1, `FTSR1` §21.6.2, `SWIER1` §21.5.5 y §21.6.3, `PR1` §21.6.21, `IMR1`/`EMR1` §21.6.19/§21.6.20. |
| **m3** | Menor | Comentario de `ExtiSoftwareTrigger` exigía solo `RTSR1`. | Corregido para reflejar que el hardware STM32H7 admite `RTSR1` o `FTSR1`. |
| **m4** | Menor | Modificación de trigger en líneas compartidas no documentada. | Documentado en [gpio_bm.h](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L343-L347) que un núcleo puede alterar los flancos de una línea activa. |
| **m5** | Menor | Falta `#error` si `DUAL_CORE` sin núcleo definido; segundo `ExtiDisable` retorna `ERR_CONFLICT`. | Añadido `#error` condicional en [gpio_bm.h](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L24-L26); documentado comportamiento de segundo deinit. |
| **m6** | Menor | Selftest sin pulls en PE3/PE4; foto incompleta; sin medición de tiempo de `pre()`; mock dependiente de `hasattr`. | Se añadieron pull-downs en PE3/PE4 durante los pasos F2; foto ampliada con `apb4enr[2]`, `pr1[2]`, `swier1` sin forzar `SYSCFGEN`; `sync()` cronometra e imprime la duración de `pre()`; mock totalmente independiente sin atributos en código de producción. |
| **m7** | Menor | Errata en nombre de `prj_02_eth_lwip_dualcore`, PATH de herramientas, y escritura tardía de capturas. | Nombre corregido a `prj_02_eth_lwip_dualcore`; PATH resuelto con búsqueda automática en `C:\ST`; capturas transmitidas línea a línea en tiempo real (`flush`). |

---

## 3. Evidencia de pruebas sin placa (Capturas de commit `bc7e64d`)

### 3.1. Compilación y Regresión T18 (`gpio_f2_checks_bc7e64d.txt`)
- **Comando:** `py tools/gpio_f2_checks.py --test all --output 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_bc7e64d.txt`
- **Árbol de trabajo:** Limpio (`working tree clean: PASS`).
- **Control worktree:** Limpio en commit `3df160a` (`control commit 3df160a: PASS`, `control worktree clean (--ignore-submodules): PASS`).
- **Matriz de compilación:** 8/8 imágenes compilaron con éxito mediante `build_all.ps1 -Clean` en 287.6 s (`CHECK build_all 8/8 (-Clean): PASS`).
- **Selftest-gpio:**
  - `prj_01_template_dualcore`: 283 pasos compilados, 0 advertencias (`PASS`).
  - `prj_04_adc_dma_bm_eth`: 327 pasos compilados, 0 advertencias (`PASS`).
  - `T1 VERDICT: PASS`.
- **Congelamiento de cabeceras:**
  - `gpio_bm.h`, `gpio_hal.h`, `gpio_ll.h`: solo adiciones (`PASS`).
  - Control negativo con mutación de firma: detectado y rechazado (`PASS`).
- **Handlers en driver:** 0 handlers en `01_shared_drivers/00_gpio/` (`PASS`); control negativo con inyección detectado (`PASS`).
- **Handlers en producción:** 0 handlers fuertes `EXTI*_IRQHandler` (`T`/`t`) en las 16 imágenes de producción (`PASS`); control positivo en `selftest-gpio` con 2 handlers fuertes presentes (`PASS`); control negativo con inyección detectado (`PASS`).
- **Estado estático desde `.map`:**
  - CM7: `.data = 0 B`, `.bss = 8 B` (`PASS`).
  - CM4: `.data = 0 B`, `.bss = 8 B` (`PASS`).
  - Control negativo con `.data = 4 B`, `.bss = 12 B`: detectado y rechazado (`PASS`).
- **Paridad de símbolos en producción:**
  - 16/16 imágenes de producción evaluadas con `nm -S` contra el control `3df160a` mostrando paridad exacta en símbolos `GPIO_BM_*` y tamaño en bytes (`PASS`).
  - Control negativo con inyección de `GPIO_BM_ExtiConfig`: detectado y rechazado (`PASS`).
- **Veredicto:** `T18 VERDICT: PASS`, `OVERALL VERDICT: PASS`.

### 3.2. Simulación del host y Analizador Lógico (`gpio_f2_mock_bc7e64d.txt`)
- **Comando:** `py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --mock-test --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_bc7e64d.txt`
- **Fase 1 (Seguridad S3):** 11 combinaciones prohibidas rechazadas por la guarda antes de la DLL (`PASS`); 5 combinaciones permitidas aceptadas (`PASS`).
- **Fase 2 (Sesión completa de pasos F2):** 25/25 checks superados (`PASS`):
  - `T11_CM7_RISING`: 200 pulsos enviados en DIO8, 200 respuestas en DIO9 medidos en LA (`PASS`).
  - `T11_CM7_FALLING`: 200 pulsos enviados en DIO8, 200 respuestas en DIO9 medidos en LA (`PASS`).
  - `T11_CM7_BOTH`: 400 pulsos enviados en DIO8, 400 respuestas en DIO9 medidos en LA (`PASS`).
  - `T11_CM7_EVENT`: 200 pulsos enviados en DIO8, 0 respuestas en DIO9 medidos en LA (`PASS`).
  - `T11_CM4_RISING`: 200 pulsos enviados en DIO10, 200 respuestas en DIO11 medidos en LA (`PASS`).
  - `T11_CM4_FALLING`: 200 pulsos enviados en DIO10, 200 respuestas en DIO11 medidos en LA (`PASS`).
  - `T11_CM4_BOTH`: 400 pulsos enviados en DIO10, 400 respuestas en DIO11 medidos en LA (`PASS`).
  - `T11_CM4_EVENT`: 200 pulsos enviados en DIO10, 0 respuestas en DIO11 medidos en LA (`PASS`).
  - `T12 independent routing`: PE3→DIO9=200, PE4→DIO11=200 en LA (`PASS`).
  - `T12 shared line routing`: PE3→DIO9=200 y DIO11=200 en LA (`PASS`).
  - `T12 isolation control`: PE3→DIO9=200 y DIO11=0 en LA (`PASS`).
  - `T13_CM7 latency`: 200/200 flancos de T11 (mín=0.15 µs, p50=0.15 µs, máx=0.15 µs ≤ 10 µs) (`PASS`).
  - `T13_CM7 control`: 0 respuestas en DIO9 tras deshabilitación (`PASS`).
  - `T13_CM4 latency`: 200/200 flancos de T11 (mín=0.15 µs, p50=0.15 µs, máx=0.15 µs ≤ 10 µs) (`PASS`).
  - `T13_CM4 control`: 0 respuestas en DIO11 tras deshabilitación (`PASS`).
  - `T14(a) pending clear`: PR1 antes=0x08, después=0x00, conteo ISR=0 (`PASS`).
  - `T14(a) control uncleared`: PR1=0x08, conteo ISR=1 (`PASS`).
  - `T14(b) conflict`: `ERR_CONFLICT` (6) con foto de registros idéntica (`PASS`).
  - `T14(b) control free line`: enrutamiento a `GPIOD` exitoso (`PASS`).
  - `T14(b) restore PE`: restauración a `PE` exitosa (`PASS`).
  - `T14(c) deinit rule 6`: compartido mantenido=1, último limpiado=1 con trigger `BOTH` (`PASS`).
  - `T14(d) software trigger`: 100 eventos con bit 0 verificado en `ExtiTakePending` (`PASS`).
  - `T14(d) control masked`: 0 eventos con línea enmascarada (`PASS`).
  - `T14 EXTI features`: veredicto firmware global PASS (`PASS`).
  - `T17 compensation cell`: rechazo sin CSI, READY=1 con CSI y restauración verificada (`PASS`).
- **Fase 3 (Control negativo de subconteo):** Detección inmediata de subconteo de firmware (199 vs 200) marcada como FAIL (`PASS`).
- **Tiempos de `pre()`:** Registro explícito en cada sincronización (e.g., 230–496 ms), garantizando amplio margen frente al timeout de 10 s del firmware.
- **Veredicto:** `HOST VERDICT=PASS`.

---

## 4. Estado de las pruebas en hardware

Como se establece en el protocolo de desarrollo, el implementador no tiene acceso a hardware. Los resultados de la corrida de diagnóstico previa ejecutada en silicio por el auditor demostraron que:
- T11 a T14 y T17(a) funcionaban correctamente en hardware con latencias excelentes de 0.10 a 0.24 µs.
- Las causas del fallo en hardware previo (bloqueo en sección de T17 y timeout de cadena) han sido corregidas en el código fuente (B1 y B2).

La verificación final en placa queda formalmente pendiente de ejecución por el auditor con las siguientes herramientas preparadas:

### Comandos de ejecución en banco para el auditor
```powershell
# 1. Posicionarse en la raíz del repositorio
cd C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\stm32h755

# 2. Flashear la imagen de selftest (commit bc7e64d)
py tools/flash.py --sn 0016000C4142501220353451 --preset selftest-gpio

# 3. Primera ejecución completa del banco con AD2 (T0 a T17)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_bc7e64d_1.txt

# 4. Segunda ejecución completa del banco (verificación de repetibilidad)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_bc7e64d_2.txt

# 5. Flashear imagen de producción y correr warm resets
py tools/flash.py --sn 0016000C4142501220353451 --preset release
py tools/run_warm_resets.py --project prj_01 --preset release
```

---

F1 ENTREGADA PARA AUDITORIA bc7e64d
