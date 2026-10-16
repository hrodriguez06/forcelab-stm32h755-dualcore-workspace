# Instrucciones para el implementador de `01_led` L1/L2

Oct 16, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta:
- `led_bm.c` exige las seis macros de pines con un `#error` cada una y comprueba el rango de los bits;
- los `board_def.h` de `prj_02`, `prj_03` y `prj_04` pasan a `_BIT`;
- la herramienta `tools/led_l1l2_checks.py`.

`led_bm.h` no cambia. Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de 2.2, que son absolutas.

---

## 1. Tu papel

Implementas y pruebas sin placa. No auditas:
- el estado de tu entrega es "entregado para auditoría";
- no toques `AUDIT.md`;
- no afirmes nada que no demuestre una captura commiteada o un `archivo:línea` que hayas comprobado contra HEAD;
- T4 queda NO VERIFICADA hasta que la ejecute el auditor.

---

## 2. Qué puedes tocar

### 2.1 Solo esto

| Ruta | Propósito |
| --- | --- |
| `01_shared_drivers/01_led/led_bm.c` | H1 |
| `02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h`, el de `prj_03_adc_dma_eth` y el de `prj_04_adc_dma_bm_eth` | H2: solo el bloque de los LED |
| `tools/led_l1l2_checks.py` (nuevo) | H3 |
| `01_shared_drivers/01_led/docs/led_l1l2_*` | Tus capturas |
| `01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md` | H4 |
| `01_shared_drivers/01_led/README.md` | Solo en el cierre, cuando el auditor lo autorice |

### 2.2 Prohibido

| Qué | Por qué |
| --- | --- |
| `led_bm.h`, cualquier otro driver, `prj_01` (ya define `_BIT`), `prj_eth_lwip_node` | Fuera de alcance |
| Programar, resetear o leer la placa (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`) | No tienes acceso a la placa; T4 la ejecuta el auditor con tu herramienta |
| Escribir dentro del árbol durante una prueba | Los `board_def.h` mutados de T2 y la salida alterada de T3 van en un directorio temporal fuera del repo |
| Editar capturas commiteadas, hacer merge, borrar ramas o documentos de proceso | Integridad y papel del responsable |

---

## 3. Cómo trabajar

- **Rama:** `driver/led-l1l2`, desde `main` en `9dc6896`. El control es `9dc6896`; la herramienta crea su worktree y lo compila dentro de la ejecución.
- **Commits:** uno por objetivo, en inglés e imperativo, desde bash con heredoc (PowerShell añade un BOM).
- **Entorno:** `.\tools\env.ps1` en cada consola nueva; `py`, nunca `python`; solo la biblioteca estándar.
- **El `#error`:** el mismo texto y la misma forma que en `08_eth/eth_hal.c:34-56`.
- **La herramienta** (`--test {t1,t2,t3,t4,all}`, `--output`):
  - comprueba el árbol limpio y que el control esté en `9dc6896` y limpio;
  - escribe la captura línea a línea, con argv y HEAD;
  - cada control negativo pasa por la misma función que la comprobación real, con una entrada mutada temporal;
  - un error de herramienta, un archivo que falta o una salida vacía es FAIL, nunca PASS;
  - **T4:** flashea con el `tools/flash.py` de cada proyecto y `--sn 0016000C4142501220353451`, espera el arranque y lee por SWD solo con `mode=HOTPLUG` y `-r32`. Nunca escribe registros.
- **Decisiones:** el responsable aprobó las tres el 2026-10-16: 1A, 2A y 3. Aplícalas.

---

## 4. Orden recomendado

1. H1 y H2, y compila la matriz.
2. H3: T1–T3, con sus controles negativos fallando como deben.
3. Commitea las capturas de T1–T3 (sin placa).
4. **H4:** el informe, que incluye:
   - qué cambió, con `archivo:línea`;
   - los resultados de T1–T3 con sus capturas;
   - el comando exacto de T4 para el auditor;
   - lo no verificado.
5. **Entrega:** árbol limpio. La última línea del informe es exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`.
