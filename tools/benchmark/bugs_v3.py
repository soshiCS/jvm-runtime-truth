"""
Bug scenario definitions for the v3 (single-bug, strong-discovery) benchmark.

Design goal: one scenario where the discovery cost difference between
Agent A (no causality) and Agent B (causality) is large and unambiguous.

The bug lives in one of 50 opaque-named processor classes (Processor001–Processor050).
Routing is multi-level: a config-driven segment resolution feeds a polynomial rolling hash
that selects the processor class name. The hash requires 32-bit Java int overflow semantics
to simulate correctly, making static reconstruction unreliable without executing the code.

Agent A must trace:
  EngineController → DispatchEngine → SegmentResolver → segment-registry.properties (KX → PRM)
  → ProcessorSelector (reads selector-config.properties for seed=44)
  → simulate: h = 44; for each char in "PRMWIRE_TRANSFER": h = h*31 + c  [32-bit overflow, 16 iters]
  → (abs(h) % 50) + 1 → 37 → Processor037.java

Agent B calls causality_reflection (or causality_polymorphic):
  → DispatchEngine.dispatch @ BCI 73/85 → Processor037
  → reads Processor037.java directly.

Key anti-shortcut properties:
- 50 processors with no semantic names (Processor001–Processor050)
- 11 processors use FEE_RATE = 0.05 (10 INT_WIRE handlers + Processor037 incorrectly)
  so grep-for-0.05 returns 11 results — cannot identify the culprit
- The hash computation requires simulating 16 iterations of 32-bit overflow arithmetic
  — an LLM doing this in text is likely to compute the wrong processor number
- Server log never names the processor class
- selector-config.properties only provides the seed; it does NOT list which processor
  handles which route — the route-to-processor mapping exists nowhere as a static table
"""

BUG1V3_INITIAL = """\
A payment processing service is returning incorrect fees.

FAILING REQUEST
  POST /api/process?accountId=ACC-4471-KX&operation=WIRE_TRANSFER&amount=10000.00

EXPECTED RESPONSE
  {"fee": 30.00, "netAmount": 9970.00, "status": "PROCESSED"}
  (Wire transfers use a 0.3% processing fee for this account tier)

OBSERVED RESPONSE
  {"fee": 500.00, "netAmount": 9500.00, "status": "PROCESSED"}
  (Fee is 5% instead of 0.3% — 16.7x higher than expected)

No exception is thrown. The response is structurally valid JSON.

Server log:
  [ENGINE] Processing: accountId=ACC-4471-KX op=WIRE_TRANSFER amount=10000.00
  [PROC] Result: fee=500.00 netAmount=9500.00 status=PROCESSED

The service routes each request to one of many processing components at runtime.
The server log does not identify which component was selected or executed.

Investigate the root cause. Application source is in:
  src/main/java/com/example/demo/engine/
  src/main/resources/routing/\
"""

# Pre-captured causality API output for the /api/process endpoint.
# This is what the ManyCore JVM produces when the request runs under instrumentation.
#
# Sections present:
#   /causality/reflection       — identifies Processor037 via reflection constructor
#   /causality/polymorphic      — identifies Processor037 via invokeinterface
#   /causality/hidden           — no hidden classes in this scenario
#   /causality/chain            — full dispatch chain for DispatchEngine.dispatch bci=73
#   /api/artifact               — bytecode artifact metadata for Processor037
#   /javap                      — javap -c -p -verbose output for Processor037.class
#
# The route-to-processor mapping is NOT in any static config table — it is computed
# at runtime by ProcessorSelector.select() via a polynomial rolling hash with 32-bit
# overflow. Without running the code, the agent cannot determine which processor
# handles PRM+WIRE_TRANSFER from config files alone.
CAUSALITY_BUG1V3 = """\
=== /causality/reflection (filtered to com/example/demo) ===
{
  "count": 1,
  "explanation": "Found 1 reflection callsite in the engine dispatch path. DispatchEngine.dispatch resolves a processor class name at runtime via a polynomial hash selector, then instantiates it through Class.forName + getDeclaredConstructor().newInstance(). The class name is computed — it is not visible in any static config table.",
  "reflection_callsites": [
    {
      "category": "reflection_constructor_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/engine/DispatchEngine",
      "source_method": "dispatch",
      "source_bci": 73,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/demo/engine/proc/Processor037",
      "target_method": "<init>",
      "target_descriptor": "()V",
      "note": "Processor037 was the class selected by ProcessorSelector.select(segment=PRM, operation=WIRE_TRANSFER) and instantiated via reflection for this request"
    }
  ]
}

=== /causality/polymorphic (filtered to com/example/demo) ===
{
  "count": 1,
  "explanation": "Found 1 polymorphic (invokeinterface) callsite. The Processor.apply() call in DispatchEngine.dispatch resolved to a single concrete target for this request.",
  "polymorphic_callsites": [
    {
      "category": "invokeinterface",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/engine/DispatchEngine",
      "source_method": "dispatch",
      "source_bci": 85,
      "source_opcode": "invokeinterface",
      "static_label": "observed_single_target",
      "target_class": "com/example/demo/engine/proc/Processor037",
      "target_method": "apply",
      "target_descriptor": "(Lcom/example/demo/engine/model/ProcessingContext;)Lcom/example/demo/engine/model/ProcessResult;",
      "note": "Processor037.apply was the concrete implementation called for accountId=ACC-4471-KX, operation=WIRE_TRANSFER"
    }
  ]
}

=== /causality/hidden ===
{
  "count": 0,
  "hidden_classes": [],
  "explanation": "No hidden classes found. All 50 processor classes (Processor001-Processor050) are standard user-defined classes loaded from the application JAR. Lambda implementations and defineHiddenClass-created classes would appear here. For a lambda-based bug, the source lambda method would be listed here with a stable artifact_crc."
}

=== /causality/chain (DispatchEngine.dispatch bci=73) ===
{
  "callsite": "com/example/demo/engine/DispatchEngine.dispatch@bci=73",
  "chain": [
    {
      "type": "callsite",
      "class": "com/example/demo/engine/DispatchEngine",
      "method": "dispatch",
      "bci": 73,
      "opcode": "invokevirtual",
      "category": "reflection_constructor_invoke",
      "static_label": "observed_single_target"
    },
    {
      "edge": "CALLSITE_TARGET",
      "id": "method::com/example/demo/engine/proc/Processor037::<init>::()V::0x0001",
      "attrs": {
        "target_class": "com/example/demo/engine/proc/Processor037",
        "target_method": "<init>",
        "target_descriptor": "()V",
        "evidence": "OBSERVED_ONLY"
      }
    }
  ],
  "explanation": "Causality chain for DispatchEngine.dispatch@bci=73: 1 direct target edge (CALLSITE_TARGET). The reflection constructor invokevirtual at BCI 73 instantiated Processor037 for the request accountId=ACC-4471-KX, operation=WIRE_TRANSFER. This is the class that executed the fee calculation. Read its source or use artifact_javap to see its FEE_RATE constant."
}

=== /api/artifact (com/example/demo/engine/proc/Processor037) ===
{
  "class": "com/example/demo/engine/proc/Processor037",
  "loader_id": "0x0000000100000001",
  "crc": "a3f2b1c8",
  "size": 1124,
  "kind": "final",
  "load_kind": "standard",
  "hidden": false,
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_demo_engine_proc_Processor037.class",
  "_exists": true,
  "note": "Standard class loaded from JAR. Use artifact_javap to disassemble and read the FEE_RATE ConstantValue directly from bytecode."
}

=== /javap (com/example/demo/engine/proc/Processor037) ===
{
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_demo_engine_proc_Processor037.class",
  "exit_code": 0,
  "output": "Compiled from \\"Processor037.java\\"\\npublic class com.example.demo.engine.proc.Processor037 implements com.example.demo.engine.proc.Processor {\\n\\n  private static final double FEE_RATE;\\n    descriptor: D\\n    flags: (0x001a) ACC_PRIVATE, ACC_STATIC, ACC_FINAL\\n    ConstantValue: double 0.05d\\n\\n  public com.example.demo.engine.proc.Processor037();\\n    descriptor: ()V\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=1, locals=1, args_size=1\\n         0: aload_0\\n         1: invokespecial #1   // Method java/lang/Object.\\"<init>\\":()V\\n         4: return\\n\\n  public com.example.demo.engine.model.ProcessResult apply(com.example.demo.engine.model.ProcessingContext);\\n    descriptor: (Lcom/example/demo/engine/model/ProcessingContext;)Lcom/example/demo/engine/model/ProcessResult;\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=8, locals=5, args_size=2\\n         0: aload_1\\n         1: invokeinterface #7,  1   // InterfaceMethod com/example/demo/engine/model/ProcessingContext.amount:()D\\n         6: ldc2_w        #13       // double 0.05d   <-- FEE_RATE (BUG: should be 0.003d)\\n         9: dmul\\n        10: ldc2_w        #14       // double 100.0\\n        13: dmul\\n        14: invokestatic  #15       // Method java/lang/Math.round:(D)J\\n        17: l2d\\n        18: ldc2_w        #14       // double 100.0\\n        21: ddiv\\n        22: dstore_2\\n        23: aload_1\\n        24: invokeinterface #7,  1   // InterfaceMethod com/example/demo/engine/model/ProcessingContext.amount:()D\\n        29: dload_2\\n        30: dsub\\n        31: ldc2_w        #14       // double 100.0\\n        34: dmul\\n        35: invokestatic  #15       // Method java/lang/Math.round:(D)J\\n        38: l2d\\n        39: ldc2_w        #14       // double 100.0\\n        42: ddiv\\n        43: dstore        4\\n        86: areturn\\n}",
  "stderr": "",
  "explanation": "Processor037.class disassembly. The ConstantValue field shows FEE_RATE = 0.05d (5%). For WIRE_TRANSFER the expected rate is 0.003 (0.3%). The constant at bytecode index 6 (ldc2_w #13 = 0.05d) is the source of the 16.7x fee overcharge. Fix: change FEE_RATE = 0.05 to FEE_RATE = 0.003 in engine/proc/Processor037.java line 7."
}
"""

SCENARIOS_V3 = {
    "bug1v3": {
        "id":           "bug1v3",
        "title":        "Bug 1v3 — 50-Processor Routing: Wrong Fee Rate in Reflection-Dispatched Component",
        "initial_msg":  BUG1V3_INITIAL,
        "causality":    CAUSALITY_BUG1V3,
        "ground_truth": {
            "file":                 "engine/proc/Processor037.java",
            "line":                 7,
            "package":              "engine",
            "patch_fragment":       "0.003",
            "patch_keywords":       ["FEE_RATE", "0.003", "0.05", "Processor037", "WIRE_TRANSFER"],
            "explanation_keywords": ["Processor037", "FEE_RATE", "fee", "rate", "wire",
                                     "reflection", "routing", "dispatch", "0.05", "0.003"],
        },
    },
}
