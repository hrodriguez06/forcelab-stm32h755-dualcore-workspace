# Instrucciones para la cuarta corrección de F0 de 00\_gpio

Sep 29, 2026 · auditor · referencia: `AUDIT.md`, "Reauditoría 3 de F0" (commit `84b8845`)

La entrega `e59baec` ya no tiene bloqueantes: F0 pasa en lo técnico. Para cerrarlo faltan **dos mayores**, R2-M3 y M3, y **un menor**, R3-m1. Esta corrección es pequeña. **No toques nada más:** todo lo demás está cerrado y verificado en la placa.

**Siguen vigentes:**

- [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md);
- las tres instrucciones de corrección anteriores, donde este documento no diga otra cosa;
- en especial, §0 de [`F0_CORRECTION_3_INSTRUCTIONS.md`](F0_CORRECTION_3_INSTRUCTIONS.md): nada oculto a git, un commit por hallazgo citando el ID, y preguntar antes que rodear una regla.

---

## 1. Orden de trabajo

| Paso | Hallazgo | Qué |
| --- | --- | --- |
| 1 | R2-M3 | Tabla válida, y el host, el modelo y el firmware atados a ella de forma que un desacuerdo se vea en las pruebas |
| 2 | R3-m1 | El host ve un `ABORT` del firmware mientras espera en un handshake |
| 3 | M3 | Informe |
| 4 | Placa y entrega | §3 y §4 |

---

## 2. Qué corregir

### 2.1 R2-M3: la tabla manda de verdad

**Elige la forma (a) o la (b)** de la errata E7 (reauditoría 3) y dilo en el informe:

- **(a)** El host y el modelo recorren la tabla.
- **(b)** El host y el modelo siguen con su código, pero lo que hacen queda registrado solo y se compara con la tabla.

Con la (b), las condiciones son estas:

1. **Traza automática.**
   - `hs_mcu()` y `hs_host()` apuntan por sí mismos `("PRIM", "HS_MCU")` o `("PRIM", "HS_HOST")`.
   - `LineReader.expect()` apunta `("LINE", <patrón recibido>)`.
   - Se acaban las llamadas `rec("…")` a mano para primitivas y líneas.
   - Los puntos de medida (`MEASURE`) pueden seguir apuntándose a mano, con su nombre.
2. **El patrón que usa el host es el de la tabla.** El host lo obtiene de la tabla (por ejemplo, `table_pattern("step6_buschk_up")`) en lugar de escribir su propia expresión.
3. **Una prueba compara la traza completa con la tabla,** ítem a ítem: tipo, primitiva y patrón, carácter a carácter. Lo mismo para el modelo.

En las dos formas:

- **Patrones válidos.** Cada patrón `LINE` de la tabla casa con la línea que imprime el firmware.
  - En C, un corchete literal se escribe `\\[` dentro de la cadena, para que la cadena contenga `\[`.
  - El parser de Python tiene que **deshacer los escapes de la cadena C** antes de usarla como expresión regular, por ejemplo con `ast.literal_eval` sobre la cadena entre comillas. Hoy lee `\\[` tal cual.
- **Prueba contra una captura real.** Una prueba toma una captura de la placa commiteada (`docs/gpio_f0_selftest_<hash>.txt`; la ruta se le pasa o se elige la más reciente) y comprueba que las líneas `[m7_gpio_test]` que no son `INFO` casan, en orden, con los patrones `LINE` de la tabla. Así la tabla queda atada al firmware real, no solo al modelo.
- **Una sola fuente para el dueño del bus.** El dueño del bus que imprime el firmware (`k_step_perms`, `CM7 gpio_selftest.c:30`) sale de `GPIO_BENCH_STEPS_TABLE`, o un `_Static_assert` por paso comprueba que coinciden.
- **Mis dos comprobaciones tienen que ponerse en rojo:**
  - cambiar en la tabla la primera primitiva del paso 7 (`HS_HOST` → `HS_MCU`);
  - cambiar el texto de un patrón `LINE` de la tabla (por ejemplo, `BUSCHK up` → `BUSCHK UP`).

  Hazlas tú antes de entregar y pon en el informe qué pruebas cayeron en cada caso.

### 2.2 R3-m1: el host ve un aborto del firmware sin esperar 10 s

- Mientras espera un nivel en `hs_mcu()` o `hs_host()`, el host sigue leyendo el puerto a través del `LineReader`.
- Si llega una línea `[m7_gpio_test] ABORT …`: S2, aborto de la sesión, y la línea citada en el log.
- **Escenario nuevo en el arnés:** el modelo emite `ABORT step=7 reason=protocol` en mitad de un handshake. El host aborta antes de 1 s simulado, con FAIL, `OE` = 0 y ninguna violación eléctrica.

### 2.3 M3: el informe

Una **sección nueva**, "Respuesta a la reauditoría 3". Las anteriores no se tocan. Lleva:

- **Una tabla** con R2-M3, R3-m1 y M3: el commit, qué ejecutaste y el estado "corregido, pendiente de verificación".
- **La corrección de las afirmaciones (a)–(c)** de M3 de la reauditoría 3:
  - (a) "derivan sus secuencias" / "imposibilitando discrepancias";
  - (b) los seis nombres que no existen;
  - (c) la atribución de R2-M1 a un commit.
- **La forma elegida para R2-M3,** y el resultado de las dos comprobaciones de §2.1.
- **Una lista de verificación de identificadores:** cada nombre de función, prueba, macro, variable, archivo o mensaje que cite la sección, con la salida de `git grep -c` para cada uno. Ninguno puede dar 0.

---

## 3. En la placa

- **Si cambia el firmware** (por ejemplo, el dueño del bus desde la tabla): dos ejecuciones completas con el AD2, como en la corrección anterior: `docs/gpio_f0_selftest_<hash>.txt` y `_2.txt`. Cualquier ejecución abortada también se commitea.
- **Si el firmware no cambia:** una ejecución completa basta, porque R3-m1 cambia cómo espera el host en la placa.
- **Árbol limpio antes de cada captura,** y la salida de `git status --short` y de `git ls-files -v | grep -E '^[a-zS]'` en el informe. Las dos tienen que estar vacías.
- **Producción:** si algún cambio toca código que compila en producción, repite la comparación byte a byte con `ca569b6`.

---

## 4. Qué entregar

1. El rango de commits nuevo en `driver/gpio`, con el árbol limpio.
2. `docs/gpio_f0_unittest_<hash>.txt`: todas las pruebas, incluidas las nuevas de §2.1 y §2.2.
3. Las capturas de la placa de §3 y, si cambió el firmware, `docs/gpio_f0_build_<hash>.txt` completo.
4. `IMPLEMENTATION_REPORT.md` con la sección "Respuesta a la reauditoría 3".
5. Un aviso con el hash. Después esperas a la "Reauditoría 4" en `AUDIT.md`.

---

## 5. Qué voy a comprobar en la reauditoría 4

| Hallazgo | Cómo | Se cierra si |
| --- | --- | --- |
| R2-M3 | Mis dos comprobaciones de §2.1 y un tercer cambio mío en la tabla que no anuncio. La prueba contra la captura real, con mi propia captura | Las tres ponen pruebas en rojo; los patrones casan con las líneas reales; el dueño del bus tiene una sola fuente |
| R3-m1 | El escenario nuevo; en la placa, repito mi mutación de la tabla del paso 7 | El host aborta en cuanto llega `reason=protocol`, no 10 s después |
| M3 | `git grep` de cada identificador del informe | Ninguna afirmación sin fuente |
| A18 | Una ejecución mía completa con el AD2 si cambió el firmware o el host | Coincide con la tuya |

Si todo se cierra, F0 queda cerrado y `driver/gpio` puede ir a `main`.

---

## 6. Cuándo parar y preguntar

- Ninguna de las dos formas de §2.1 encaja con cómo está escrito el host.
- La prueba contra la captura real no se puede escribir con las capturas que hay.
- Atar el dueño del bus a la tabla cambia la imagen de producción.
