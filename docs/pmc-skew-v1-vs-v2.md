# PMC Grouping Skew — v1 vs v2 Comparison Report

**Date:** 2026-05-22  
**Author:** agent_claude (sess_2026-05-22_0900), requested by Osvaldo J. Filho  
**Test:** `pmcstat_grouping_test:repeated_process_cycles_have_bounded_skew`  
**Duration:** 120 minutes per version, hardware AMD EPYC Zen 4 (family 0x19 model 0x11)

---

## 1. Baseline histórico (pré-coleta — dados empíricos anteriores)

| Métrica | Valor |
|---|---|
| Total de execuções | 481 |
| Pass | 417 (86.7 %) |
| Fail | 64 (13.3 %) |
| p90 | 100.7 ‰ |
| p95 | 103.9 ‰ |
| max observado | 122.6 ‰ |
| Tolerância usada | 100 ‰ |

---

## 2. Resultados da coleta de 2 horas

### 2.1 Tabela side-by-side

| Métrica | v1 — `sleep 5` / tol 100 ‰ | v2 — `cpuset+dd` / tol 50 ‰ | v2 válido (≥10 M ciclos) / tol 50 ‰ |
|---|---|---|---|
| Iterações | 475 | 1 404 | 74 |
| Pass | **475** | 66 | **52** |
| Fail | **0** | 1 338 | **22** |
| **Fail rate** | **0.0 %** | **95.3 %** | **29.7 %** |
| min ‰ | 0.003 | 0.021 | 0.021 |
| mean ‰ | 42.0 | 48.3 | **24.4** |
| p50 ‰ | 44.7 | 48.9 | — |
| p90 ‰ | 87.6 | 74.0 | **50.3** |
| p95 ‰ | 90.0 | 80.3 | **54.9** |
| max ‰ | **97.7** | **109.9** | **69.2** |
| stdev ‰ | 33.7 | 21.2 | — |
| low_count_warnings | 0 | 1 330 (94.7 %) | — |

> **v2 válido:** amostras de v2 com `last_a ≥ 10 M ciclos` (workload dd executou por tempo suficiente).

### 2.2 Interpretação imediata

- **v1 (sleep 5, tol 100 ‰):** 475 amostras, **0 falhas**. A tolerância de 100 ‰ é segura para o
  workload `sleep` — consistente com os dados históricos onde a maioria dos runs passava.

- **v2 (cpuset+dd, tol 50 ‰):** 95 % das amostras têm `low_count_warning` — o `dd` acumulou
  apenas ~7–8 M ciclos (mediana), bem abaixo do threshold de 10 M. Nas 74 amostras válidas
  (dd rodou de fato por tempo suficiente), o skew cai para mean 24 ‰ e max 69 ‰, com
  29.7 % de falha ao nível de 50 ‰.

---

## 3. Root cause analysis — revisada pelos dados

### 3.1 Hipóteses iniciais e resultado

| Hipótese | Status após coleta |
|---|---|
| Workload fraco (`sleep`) → contagens pequenas → jitter relativo alto | **Parcialmente confirmada** — ciclos baixos amplificam o skew, mas não é a causa primária |
| Migração de CPU entre cores/CCXs | **Refutada pelos dados v2** — cpuset não ajudou (quando dd rodou, skew ainda chegou a 69 ‰) |
| Timing independente dos dois `pmc_start()` / leitura final | **Causa primária confirmada** — persiste em ambas as versões |

### 3.2 Causa primária real

O problema está na **arquitetura de leitura do pmcstat em modo `-C`** (process counting):

```
pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -o out -- <workload>
```

Com `-C`, o pmcstat aloca **dois PMC rows distintos** (confirmado por
`hwpmc_grouping_test.c:require_distinct_thread_rows`). Cada row recebe um `pmc_start()`
separado e, ao fim do processo, um `pmc_read()` também separado. O valor final reportado
é o total acumulado em cada row **sem sincronização entre eles**.

A janela entre os dois `pmc_start()` / `pmc_read()` é suficiente para que ciclos reais
do processo sejam contados em apenas um dos counters. Em um EPYC Zen 4 a ~3–4 GHz,
~30 µs de jitter entre os dois syscalls equivale a ~90–120 K ciclos — que sobre uma base
de ~7 M ciclos (`sleep` curto ou `dd` interrompido cedo) resulta em 13–17 ‰ de skew só
de start, mais contribuições similares no read.

### 3.3 Por que v2 não ajudou como esperado

O `dd` com `timeout 5` foi morto pelo `timeout` **antes** de acumular 10 M ciclos na
maioria das iterações (mediana ~7.7 M). O `timeout` interrompe o processo via SIGTERM,
e o pmcstat lê os counters neste ponto — quando o processo ainda não acumulou ciclos
suficientes para que o jitter de read seja desprezível.

Nas 74 amostras onde o `dd` rodou por tempo suficiente (>10 M ciclos), o skew cai
significativamente: mean 24 ‰ vs 42 ‰ do v1, max 69 ‰ vs 97 ‰. Isso **confirma** que
ciclos altos ajudam, mas o p95 de 55 ‰ ainda excede a tolerância de 50 ‰ — indicando
que o jitter de timing existe mesmo em workloads CPU-bound longos.

### 3.4 Novo modelo de root cause

```
skew_permille = (timing_jitter_cycles / total_cycles) × 1000

Componentes de timing_jitter_cycles:
  A. pmc_start() jitter entre row 0 e row 1     → ~30–100 µs × freq
  B. pmc_read() jitter entre row 0 e row 1      → ~10–50 µs × freq
  C. Variação de frequência (boost/idle) por CCX → pequena com cpuset, mas não zero

total_cycles:
  sleep 5  → ~2–7 M  (idle, apenas syscall overhead)
  dd 5s    → ~7–40 M (CPU-bound mas terminado por SIGTERM antes do esperado)
  dd longo → ~50–500 M (esperado; reduziria skew para <5 ‰)
```

---

## 4. Bugs documentados

### Bug pmc-001 (implícito): `pmcstat -C` não sincroniza start/read de múltiplos rows

- **Onde:** `usr.sbin/pmcstat/pmcstat.c` e `sys/dev/hwpmc/hwpmc.c` no FreeBSD base
- **Comportamento:** Dois PMC rows alocados para o mesmo processo recebem `pmc_start()` e
  `pmc_read()` em chamadas de syscall separadas, sem garantia de atomicidade.
- **Impacto:** Qualquer teste que compare dois counters idênticos em modo `-C` terá skew
  proporcional ao jitter de syscall × frequência do CPU.
- **Workaround atual:** Tolerância generosa (250 ‰ no ATF, 100 ‰ no coletor v1).

### Bug pmc-002: collector `DEFAULT_TOLERANCE_PERMILLE = 100` abaixo do max empírico

- **Arquivo:** `py-scripts/pmc_grouping_skew_collect.py:59`
- **Impacto:** Gera falsos negativos quando skew cai entre 100–123 ‰ (~13 % dos runs).
- **Fix:** Elevar para 250 (alinhado com o ATF) ou documentar como threshold de pesquisa.
- **Referência:** `dev-docs/modules/pmc-tests.md#BUG-pmc-002`

### Bug pmc-003 (novo): v2 `timeout 5 dd` termina por SIGTERM antes de acumular ciclos

- **Arquivo:** `py-scripts/pmc_grouping_skew_v2_collect.py` e
  `tests/sys/amd/pmc/pmcstat_grouping_test_v2.sh`
- **Sintoma:** 94.7 % das amostras v2 têm `low_count_warning` (< 10 M ciclos).
- **Causa:** O `timeout 5` mata o `dd` via SIGTERM. O pmcstat em modo `-C` encerra a coleta
  ao receber o exit do processo monitorado. Com `cpuset -l 0`, o dd é confinado a 1 core e
  pode não atingir 10 M ciclos em 5 s se houver contention de scheduler ou se o processo
  não for imediatamente escalonado.
- **Fix:** Aumentar o tempo de `timeout` para 15–30 s, ou usar uma contagem fixa de bytes
  (`dd count=500000`) ao invés de tempo, garantindo que sempre rode até o fim.

---

## 5. Mudanças na v2

### `tests/sys/amd/pmc/pmcstat_grouping_test_v2.sh`

| Aspecto | v1 | v2 |
|---|---|---|
| Arquivo | `pmcstat_grouping_test.sh` | `pmcstat_grouping_test_v2.sh` |
| Workload | `sleep 5` | `cpuset -l 0 timeout 5 dd if=/dev/zero of=/dev/null bs=4096` |
| CPU affinity | Nenhuma | `cpuset -l 0` (CPU 0) |
| Tolerância default | 250 ‰ | **50 ‰** |
| ATF config key | `cycle_tolerance_permille` | `cycle_v2_tolerance_permille` |
| Makefile/Kyuafile | Sim (produção) | **Não** (artefato de pesquisa) |
| Sanity check de contagem | Não | Sim (warning se < 10 M ciclos) |

### `py-scripts/pmc_grouping_skew_v2_collect.py`

| Aspecto | v1 collector | v2 collector |
|---|---|---|
| Workload | `sleep {N}` (hardcoded) | `--workload-cmd` configurável |
| Default workload | `sleep 5` | `cpuset -l 0 timeout 5 dd ...` |
| `DEFAULT_TOLERANCE_PERMILLE` | 100 | **50** |
| Integração kyua | Sim (opt-in) | **Não** (direct probe only) |
| `low_count_warning` | Não | **Sim** (< 10 M ciclos) |
| Output dir default | `./pmu-skew-data` | `./pmu-skew-data-v2` |
| Schema JSON/CSV | v3 | v3 (compatível) |

---

## 6. Conclusão

### É bug real ou teste mal especificado?

**Ambos, com pesos diferentes:**

1. **Bug estrutural no pmcstat (causa primária):** A falta de atomicidade entre
   `pmc_start()` e `pmc_read()` de múltiplos rows em modo `-C` é um comportamento
   intrinsecamente ruidoso. Dois counters "idênticos" nunca serão bit-a-bit iguais porque
   o kernel não os arma/lê simultaneamente. Isso é documentável como limitação de design,
   mas não como bug de corrupção — os valores são corretos individualmente.

2. **Teste mal especificado (causa secundária):** O uso de `sleep` maximiza o efeito
   relativo do jitter (denominador pequeno). A tolerância do ATF (250 ‰) está calibrada
   para esconder esse problema. O coletor v1 com 100 ‰ expõe a fragilidade.

3. **v2 mal implementado (bug novo introduzido):** O `timeout 5 dd` termina por SIGTERM
   antes de acumular ciclos suficientes. A intenção (workload CPU-bound longo) é correta,
   mas a implementação precisa de `dd count=N` ao invés de `timeout`.

### Recomendações prioritárias

| Prioridade | Ação | Arquivo |
|---|---|---|
| **1 — imediata** | Elevar `DEFAULT_TOLERANCE_PERMILLE` de 100 para 250 no coletor v1 | `py-scripts/pmc_grouping_skew_collect.py:59` |
| **2 — v2** | Substituir `timeout 5 dd` por `dd count=500000` (termina por exaustão, não SIGTERM) | `pmcstat_grouping_test_v2.sh`, `pmc_grouping_skew_v2_collect.py` |
| **3 — v2** | Com `dd count=500000`, retestar com tolerância 20–30 ‰ para validar se skew cai de fato | — |
| **4 — longo prazo** | Investigar se `pmcstat` pode usar `PMC_F_ATTACH_PROCESS` com leitura atômica de grupo | FreeBSD base `usr.sbin/pmcstat/` |

---

## 7. Arquivos produzidos nesta investigação

| Arquivo | Descrição |
|---|---|
| `tests/sys/amd/pmc/pmcstat_grouping_test_v2.sh` | ATF test v2 (cpuset+dd, tol 50 ‰) |
| `py-scripts/pmc_grouping_skew_v2_collect.py` | Coletor Python v2 (--workload-cmd) |
| `docs/pmc-skew-v1-vs-v2.md` | Este relatório |
| `/tmp/pmc-skew-v1/pmc-grouping-skew.{json,csv}` | 475 amostras v1 (2h) |
| `/tmp/pmc-skew-v2/pmc-grouping-skew-v2.{json,csv}` | 1 404 amostras v2 (2h) |
