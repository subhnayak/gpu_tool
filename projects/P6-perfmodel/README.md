# P6 — GPU Analytical Performance Model

## Goal

Build an analytical model that **predicts GPU kernel runtime**, then validate it against measured results. This exercises the "empower GPU architects to understand application performance today and model industry-leading performance for tomorrow" bullet in the job description.

## Theory

The model combines three foundational concepts:

### 1. Roofline Model
- X-axis: Arithmetic Intensity (FLOP/byte)
- Y-axis: Achievable performance (GFLOPS)
- Performance = min(AI × bandwidth, peak_flops)
- Ridge point = peak_flops / bandwidth

### 2. Occupancy
- How many warps can run concurrently on an SM
- Limited by: registers, shared memory, block count, warp count
- Higher occupancy → better latency hiding → higher achieved bandwidth

### 3. Little's Law
- To sustain bandwidth B with latency L: need N = B×L/S concurrent requests
- If occupancy can't provide enough requests → latency-bound

## Files

| File | Purpose |
|------|---------|
| `gpumodel/machine.py` | GPU hardware model (SM count, clocks, bandwidth, limits) |
| `gpumodel/kernel.py` | Kernel descriptor (grid, block, regs, bytes, FLOPs) |
| `gpumodel/occupancy.py` | Occupancy calculator with binding-constraint analysis |
| `gpumodel/roofline.py` | Roofline model, classification, ASCII + matplotlib plots |
| `gpumodel/model.py` | Performance predictor with full `explain()` reasoning |
| `gpumodel/validate.py` | Compare predictions against measured results |
| `examples/predict_p3.py` | Worked predictions for P3 kernels (interview answers) |
| `examples/what_if.py` | Architect-style what-if analysis |
| `tests/test_model.py` | Tests with hand-computed expected values |

## Usage

```bash
# Run worked predictions for P3 kernels
python examples/predict_p3.py

# Run what-if analysis
python examples/what_if.py

# Run tests
python tests/test_model.py
# Or with pytest:
pytest tests/test_model.py -v
```

## Feeding in Measured Data from P3

Export P3 results as JSON:
```json
[
  {"name": "vector_add", "time_ms": 0.05, "bytes_read": 8388608, "bytes_written": 4194304, "flops": 1048576, "block_size": 256, "regs_per_thread": 16},
  {"name": "transpose", "time_ms": 0.08, "bytes_read": 4194304, "bytes_written": 4194304, "flops": 0, "block_size": 256}
]
```

Then run validation:
```python
from gpumodel.machine import MachineModel
from gpumodel.validate import load_measured_json, validate
machine = MachineModel(...)  # your GPU
data = load_measured_json("p3_results.json")
print(validate(machine, data))
```

## Acceptance Criteria

1. **Occupancy calculator matches hand-computed values** — three worked examples in tests pass
2. **Roofline correctly classifies** memory-bound vs compute-bound kernels
3. **Predictions within ~20-30%** for memory-bound kernels (vector add, transpose)
4. **Model honestly reports its limitations** — the `explain()` output states caveats
5. **What-if analysis shows expected behavior**: more bandwidth helps memory-bound kernels, more compute helps compute-bound kernels
6. **All tests pass**: `pytest tests/test_model.py` returns 0

### Honest Limitations

- A simple analytical model is typically within **~20-30%** for well-behaved memory-bound kernels
- **Much worse (50%+)** for latency-bound, cache-sensitive, or control-flow-heavy kernels
- **Understanding WHY it is wrong** is the actual learning objective
- The model does NOT capture: L1/L2 cache effects, bank conflicts, warp divergence, instruction scheduling, or PCIe transfer overhead

## Interview Questions This Project Answers

| Question | Where |
|----------|-------|
| Explain the roofline model | `roofline.py`, `predict_p3.py` |
| How do you calculate occupancy? | `occupancy.py` — whiteboard-ready |
| What is arithmetic intensity for vector add? | `predict_p3.py` — worked derivation |
| What limits GPU kernel performance? | `model.py` — three walls |
| How would you predict kernel runtime? | `model.py` — full model equation |
| What-if: double the memory bandwidth? | `what_if.py` — worked report |
| When does the model fail? | `validate.py`, `model.py` — honest analysis |
| What is Little's Law for GPUs? | `model.py` — memory-level parallelism |
