"""
Bug scenario definitions for the v4 (runtime-artifact) benchmark.

Design goal: demonstrate the advantage of captured runtime bytecode artifacts
over static source inspection.  The bug requires TWO runtime capabilities
that Agent A structurally cannot replicate:

  1. Identifying WHICH generated class ran (class name is not in any log).
  2. Reading WHAT the generated class contains (no .java source file exists).

The application uses a hash-based profile selector (FeeRuleCompiler) that:
  - Computes index = polynomial_hash("premium" + "TRANSFER", seed=44) % 50 + 1 → 9
  - Reads fee-config/rate-profiles.properties: profile.009 = 0.025  ← THE BUG
  - Uses ByteBuddy to generate a FeeCalculatorBase subclass with 0.025 baked in
  - Generated class name: FeeCalculatorBase$Profile009$<hash>
    (profile index is in the name so monitoring tools can correlate it)

The wrong rate 0.025 exists in NO source file that can be read without the hash.
rate-profiles.properties has 10 entries with 0.025 (profiles 002, 007, 009, 015,
023, 031, 037, 043, 047, 050) — grep for 0.025 returns 10 results and cannot
identify the culprit without knowing the hash result.

Agent A's blocker:
  - Must simulate 15 iterations of h = h*31 + c with 32-bit Java int overflow
    for input "premiumTRANSFER" (same arithmetic barrier as V3's 16-iteration hash)
  - LLM arithmetic error → wrong profile index → reads wrong config entry → fails
  - Even correct source tracing ends at "one of 10 entries with 0.025" with no
    further way to narrow down

Agent B's path (2–3 tool calls):
  1. causality_reflection → generated class is FeeCalculatorBase$Profile009$t3yK6fHm
                            name immediately reveals profile index = 9
  2. artifact_javap       → getRate() returns ldc2_w 0.025d (confirms the bug)
  3. submit_diagnosis     → fee-config/rate-profiles.properties line 11, change to 0.003

Key anti-shortcut properties:
  - Wrong rate 0.025 exists in NO Java source or log output
  - Grep for 0.025 in config returns 10 results — cannot identify culprit
  - selector-config.properties shows seed=44 but NOT which profile index that maps to
  - Generated class name not in any log (only in causality API output)
  - Hash requires simulating 15 iterations of 32-bit overflow arithmetic
"""

BUG1V4_INITIAL = """\
A fee override service is applying incorrect transaction fees for premium accounts.

FAILING REQUEST
  POST /api/fee?accountId=ACC-PREM-8821&operation=TRANSFER&amount=5000.00

EXPECTED RESPONSE
  {"fee": 15.00, "netAmount": 4985.00, "status": "OK"}
  (Premium account transfer: 0.3% fee)

OBSERVED RESPONSE
  {"fee": 125.00, "netAmount": 4875.00, "status": "OK"}
  (Fee is over 8× higher than expected — 2.5% instead of 0.3%)

No exception is thrown. The response is structurally valid JSON.

Server log:
  [FEE] Processing: accountId=ACC-PREM-8821 operation=TRANSFER amount=5000.00
  [FEE] Override rule compiled for request
  [FEE] Result: fee=125.00 netAmount=4875.00 status=OK

The fee calculation is driven by an override rule that is applied at runtime.
The log does not identify the rule, the rate applied, or the configuration entry used.

Investigate the root cause. Application source is in:
  src/main/java/com/example/demo/fee/
  src/main/resources/fee-config/\
"""

# Pre-captured causality API output for POST /api/fee with accountId=ACC-PREM-8821.
# This is what the ManyCore JVM produces when the request runs under instrumentation.
#
# Sections present:
#   /causality/reflection    — identifies FeeCalculatorBase$Profile009$t3yK6fHm via
#                              reflection constructor; name encodes profile index 9
#   /causality/polymorphic   — identifies same generated class via invokeinterface
#   /causality/hidden        — count=0: ByteBuddy INJECTION creates regular classes
#   /causality/chain         — dispatch chain for FeeRuleCompiler.compile bci=97
#   /api/artifact            — artifact record for the generated class
#   /javap                   — javap output showing getRate() returns 0.025d
#
# The rate 0.025 appears in NO Java source file — it exists only in the generated
# bytecode. The generated class name encodes "Profile009", identifying the config
# entry to fix (fee-config/rate-profiles.properties line 11: profile.009=0.025).
CAUSALITY_BUG1V4 = """\
=== /causality/reflection (filtered to com/example/demo) ===
{
  "count": 1,
  "explanation": "Found 1 reflection callsite in the fee override dispatch path. FeeRuleCompiler.compile() uses a polynomial rolling hash to select a rate profile index, looks up the rate from rate-profiles.properties, then uses ByteBuddy to generate a FeeCalculatorBase subclass with the rate baked in. The generated class is instantiated via getDeclaredConstructor().newInstance(). The class name includes the profile index so monitoring tools can correlate it to the config entry.",
  "reflection_callsites": [
    {
      "category": "reflection_constructor_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/fee/rule/FeeRuleCompiler",
      "source_method": "compile",
      "source_bci": 97,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm",
      "target_method": "<init>",
      "target_descriptor": "()V",
      "note": "Generated FeeOverrideHandler subclass for profile index 9 (selected for tier=premium, operation=TRANSFER). Profile index is encoded in the class name. Use artifact_javap to read the baked-in rate constant from the generated bytecode."
    }
  ]
}

=== /causality/polymorphic (filtered to com/example/demo) ===
{
  "count": 1,
  "explanation": "Found 1 polymorphic (invokeinterface) callsite. The FeeOverrideHandler.compute() call in FeeEngine.compute() resolved to a single concrete target for this request.",
  "polymorphic_callsites": [
    {
      "category": "invokeinterface",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/fee/FeeEngine",
      "source_method": "compute",
      "source_bci": 42,
      "source_opcode": "invokeinterface",
      "static_label": "observed_single_target",
      "target_class": "com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm",
      "target_method": "compute",
      "target_descriptor": "(D)D",
      "note": "Profile009 handler was the concrete FeeOverrideHandler for accountId=ACC-PREM-8821, operation=TRANSFER. compute() is inherited from FeeCalculatorBase which delegates to getRate() — override in generated class returns the baked-in constant."
    }
  ]
}

=== /causality/hidden ===
{
  "count": 0,
  "hidden_classes": [],
  "explanation": "No hidden classes found. FeeRuleCompiler uses ByteBuddy with ClassLoadingStrategy.Default.INJECTION, which loads generated classes into the system classloader as standard user-defined classes — not hidden classes. Lambda implementations and defineHiddenClass-based generators would appear here. For the generated fee rule handler, use causality_reflection to find the class name (which encodes the profile index), then artifact_javap to read the baked-in rate constant."
}

=== /causality/chain (FeeRuleCompiler.compile bci=97) ===
{
  "callsite": "com/example/demo/fee/rule/FeeRuleCompiler.compile@bci=97",
  "chain": [
    {
      "type": "callsite",
      "class": "com/example/demo/fee/rule/FeeRuleCompiler",
      "method": "compile",
      "bci": 97,
      "opcode": "invokevirtual",
      "category": "reflection_constructor_invoke",
      "static_label": "observed_single_target"
    },
    {
      "edge": "CALLSITE_TARGET",
      "id": "method::com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm::<init>::()V::0x0001",
      "attrs": {
        "target_class": "com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm",
        "target_method": "<init>",
        "target_descriptor": "()V",
        "evidence": "OBSERVED_ONLY"
      }
    }
  ],
  "explanation": "Causality chain for FeeRuleCompiler.compile@bci=97: 1 direct target edge (CALLSITE_TARGET). The reflection constructor invokevirtual at BCI 97 instantiated FeeCalculatorBase$Profile009$t3yK6fHm for the request accountId=ACC-PREM-8821, operation=TRANSFER. Profile009 in the class name identifies the rate-profiles.properties entry to check. Use artifact_javap to read the baked-in rate constant and confirm which config entry is wrong."
}

=== /api/artifact (com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm) ===
{
  "class": "com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm",
  "loader_id": "0x0000000100000001",
  "crc": "c9d3a1f4",
  "size": 441,
  "kind": "final",
  "load_kind": "standard",
  "hidden": false,
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_demo_fee_rule_FeeCalculatorBase$Profile009$t3yK6fHm.class",
  "_exists": true,
  "note": "ByteBuddy-generated class loaded via ClassLoadingStrategy.Default.INJECTION. No .java source file exists. Profile009 in the name maps to profile.009 in fee-config/rate-profiles.properties. Use artifact_javap to read the exact rate constant that was baked into getRate() at generation time."
}

=== /javap (com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm) ===
{
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_demo_fee_rule_FeeCalculatorBase$Profile009$t3yK6fHm.class",
  "exit_code": 0,
  "output": "Compiled from \\"net.bytebuddy.dynamic.DynamicType\\"\\npublic class com.example.demo.fee.rule.FeeCalculatorBase$Profile009$t3yK6fHm extends com.example.demo.fee.rule.FeeCalculatorBase {\\n\\n  public com.example.demo.fee.rule.FeeCalculatorBase$Profile009$t3yK6fHm();\\n    descriptor: ()V\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=1, locals=1, args_size=1\\n         0: aload_0\\n         1: invokespecial #1                  // Method com/example/demo/fee/rule/FeeCalculatorBase.\\"<init>\\":()V\\n         4: return\\n\\n  public double getRate();\\n    descriptor: ()D\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=2, locals=1, args_size=1\\n         0: ldc2_w        #7                  // double 0.025d    <-- BUG: profile.009 has wrong rate (should be 0.003)\\n         3: dreturn\\n}",
  "stderr": "",
  "explanation": "FeeCalculatorBase$Profile009$t3yK6fHm disassembly. The getRate() method at instruction 0 loads the constant 0.025d (2.5%) via ldc2_w — this was baked in at generation time from fee-config/rate-profiles.properties entry profile.009. The expected rate for a premium TRANSFER is 0.003 (0.3%), as recorded in fee-config/fee-overrides.properties (target.premium.transfer=0.003). Fix: change profile.009=0.025 to profile.009=0.003 in fee-config/rate-profiles.properties line 11."
}
"""

SCENARIOS_V4 = {
    "bug1v4": {
        "id":           "bug1v4",
        "title":        "Bug 1v4 — Hash-Selected Rate Profile: Wrong Constant in Generated Handler",
        "initial_msg":  BUG1V4_INITIAL,
        "causality":    CAUSALITY_BUG1V4,
        "ground_truth": {
            "file":                 "fee-config/rate-profiles.properties",
            "line":                 11,
            "package":              "fee",
            "patch_fragment":       "0.003",
            "patch_keywords":       ["profile.009", "0.003", "0.025", "rate-profiles"],
            "explanation_keywords": ["profile", "009", "rate", "0.025", "0.003",
                                     "hash", "Profile009", "generated", "baked"],
        },
    },
}
