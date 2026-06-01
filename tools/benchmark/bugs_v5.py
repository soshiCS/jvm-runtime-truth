"""
Bug scenario definitions for the v5 (flagship runtime truth) benchmark.

This is the V5 "Runtime Truth Decryption Challenge" — designed as the flagship
public demo for the ManyCore platform.

The scenario exercises every major runtime visibility capability in a single
request:

  /causality/reflection  — sees DecryptPipeline dispatching via Method.invoke
  /causality/hidden      — sees JDK proxy class + two lambda hidden classes
  /causality/chain       — full chain from reflection site to generated class
  /causality/polymorphic — sees invokeinterface on TransformHandler resolving
                           to the generated class
  /api/artifact          — confirms the generated class artifact is on disk
  /javap                 — reads the wrong XOR key baked into getKey() bytecode

The application story:
  A decryption service accepts ciphertext and a sessionKey. The transform selector
  computes composite = sessionKey + ":" + CRC32(ciphertext), then runs a 47-char
  polynomial rolling hash to select one of 50 transforms. The ciphertext is
  "RUNTIME SHOWS THE TRUTH" XOR 127. The active transform for
  sessionKey="550e8400-e29b-41d4-a716-446655440000" is transform 037, which
  has the wrong key (75 instead of 127) baked into its getKey() method, causing
  decryption to produce garbage.

Key anti-shortcut properties:
  - Wrong key (75) appears in 10 of the 50 transform config entries.
    Grep returns 10 results — cannot identify the active one without the hash.
  - Generated class has NO .java source file.
    artifact_javap is the only way to confirm which key was baked in.
  - Selection uses 3-component composite: sessionKey + CRC32(ciphertext) +
    INSTANCE_TOKEN (System.nanoTime() at class load — never logged).
  - INSTANCE_TOKEN is the hard barrier: it is never in any config file, log,
    or source. Agent A cannot know it without running the JVM.
  - Even ignoring INSTANCE_TOKEN, CRC32(ciphertext) over 23 bytes requires
    polynomial division that LLMs cannot reliably compute in text.
  - The class name encodes the transform index (Transform037 visible in causality
    output), allowing Agent B to bypass the hash entirely.
  - The JDK proxy and lambda hidden classes in the chain are red herrings —
    the bug is in the generated transform class, not in the proxy layer.

Agent A blocker:
  - INSTANCE_TOKEN (System.nanoTime() at class load) is never logged and
    never in any config or source file. Without it, the composite hash cannot
    be reproduced regardless of any other reasoning the agent performs.
  - This means: no amount of static source inspection can determine which of
    the 10 transforms with key=75 was active for this request.

Agent B path (3 tool calls):
  1. causality_chain (or causality_reflection) → Transform037 class name
  2. artifact_javap → getKey() returns bipush 75 (should be 127)
  3. submit_diagnosis → transform-config/transform-keys.properties line 40
"""

BUG1V5_INITIAL = """\
A decryption service is returning corrupted output for a specific session.

FAILING REQUEST
  POST /api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000
  Content-Type: application/json
  Body: {"ciphertext": "2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}

  Reproduction:
    curl -s -X POST 'http://localhost:8080/api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000' \\
      -H 'Content-Type: application/json' \\
      -d '{"ciphertext":"2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}'

EXPECTED RESPONSE
  {"status": "OK", "output": "RUNTIME SHOWS THE TRUTH", "sessionKey": "550e8400-e29b-41d4-a716-446655440000"}

OBSERVED RESPONSE
  {"status": "DECRYPTION_FAILED", "output": "66617a607d797114677c7b636714607c7114606661607c",
   "sessionKey": "550e8400-e29b-41d4-a716-446655440000"}

No exception is thrown. The service processes the request and returns a
structurally valid JSON response. The output is raw hex because it contains
non-printable bytes (the service falls back to hex when ASCII validation fails).

Server log:
  [DECRYPT] Processing sessionKey=550e8400-e29b-41d4-a716-446655440000 ciphertext=2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37
  [DECRYPT] Pipeline completed — inspecting result
  [DECRYPT] Result: status=DECRYPTION_FAILED output=66617a607d797114677c7b636714607c7114606661607c

The log does not identify the transform applied, the key used, the composite
input used for selection, or the CRC of the ciphertext.


Investigate the root cause. Application source is in:
  src/main/java/com/example/truth/
  src/main/resources/transform-config/\
"""

# Pre-captured causality API output for POST /api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000.
# This is what the ManyCore JVM produces when the request runs under instrumentation.
#
# The pipeline chain:
#   DecryptPipeline.execute()          — Method.invoke on PipelineExecutor proxy
#     → $Proxy<N> (JDK proxy, hidden)  — dispatches via InvocationHandler lambda
#       → PipelineProxy.dispatch()     — calls TransformCompiler.compile()
#         → TransformBase$Transform037$r8K3mNp2  ← generated class, no .java source
#           getKey() returns bipush 75            ← THE BUG (should be 127)
#
# The value 75 appears in NO Java source file — only in:
#   1. transform-config/transform-keys.properties entry transform.037 (one of 10 with key=75)
#   2. The generated class bytecode (confirmed by artifact_javap)
#
# The transform index 037 is not logged anywhere — only visible in causality output.
CAUSALITY_BUG1V5 = """\
=== /causality/reflection (filtered to com/example/truth) ===
{
  "count": 2,
  "explanation": "Found 2 reflection callsites in the decrypt pipeline. (1) DecryptPipeline.execute() uses Method.invoke to call the PipelineExecutor proxy. (2) TransformCompiler.doCompile() uses Constructor.newInstance() to instantiate a ByteBuddy-generated TransformBase subclass. The generated class name encodes the transform index, which maps directly to a config entry. Use artifact_javap to read the XOR key constant baked into getKey().",
  "reflection_callsites": [
    {
      "category": "reflection_method_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/truth/DecryptPipeline",
      "source_method": "execute",
      "source_bci": 9,
      "source_opcode": "invokevirtual",
      "target_class": "jdk.proxy1.$Proxy0",
      "target_method": "execute",
      "target_descriptor": "(Lcom/example/truth/model/DecryptContext;)[B",
      "note": "Method.invoke dispatches to the JDK dynamic proxy for PipelineExecutor. The proxy is a hidden class — see causality_hidden for details. The proxy's InvocationHandler lambda routes to PipelineProxy.dispatch()."
    },
    {
      "category": "reflection_constructor_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/truth/handler/TransformCompiler",
      "source_method": "doCompile",
      "source_bci": 73,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/truth/handler/TransformBase$Transform037$r8K3mNp2",
      "target_method": "<init>",
      "target_descriptor": "()V",
      "note": "Generated TransformBase subclass for transform index 037 (selected by 3-component composite hash: sessionKey + CRC32(ciphertext) + INSTANCE_TOKEN, where INSTANCE_TOKEN=System.nanoTime() at class load — never logged). Transform037 in the class name identifies the config entry. Use artifact_javap to read the XOR key baked into getKey()."
    }
  ]
}

=== /causality/polymorphic (filtered to com/example/truth) ===
{
  "count": 1,
  "explanation": "Found 1 polymorphic (invokeinterface) callsite. The TransformHandler.applyTransform() call in PipelineProxy.dispatch() resolved to a single concrete target for this request.",
  "polymorphic_callsites": [
    {
      "category": "invokeinterface",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/truth/proxy/PipelineProxy",
      "source_method": "dispatch",
      "source_bci": 12,
      "source_opcode": "invokeinterface",
      "static_label": "observed_single_target",
      "target_class": "com/example/truth/handler/TransformBase$Transform037$r8K3mNp2",
      "target_method": "applyTransform",
      "target_descriptor": "([B)[B",
      "note": "Transform037 handler was the concrete TransformHandler for sessionKey=550e8400-e29b-41d4-a716-446655440000. applyTransform() is inherited from TransformBase which XORs each byte with getKey() — the key is wrong (75 instead of 127) causing decryption failure."
    }
  ]
}

=== /causality/hidden ===
{
  "count": 3,
  "hidden_classes": [
    {
      "runtime_name": "jdk.proxy1.$Proxy0/0x0000000100200000",
      "stable_crc": "f4a1c8d2",
      "loader_id": "0x0000000100000001",
      "has_artifact": false,
      "note": "JDK dynamic proxy implementing PipelineExecutor. This proxy wraps the dispatch into PipelineProxy.dispatch() via its InvocationHandler lambda. Not the bug source — the proxy is infrastructure, not the transform logic."
    },
    {
      "runtime_name": "com.example.truth.proxy.PipelineProxy$$Lambda/0x0000000100c3a7b0",
      "stable_crc": "9b3e7f41",
      "loader_id": "0x0000000100000001",
      "has_artifact": false,
      "note": "InvocationHandler lambda for the PipelineExecutor proxy. Defined in PipelineProxy.createExecutor(). Routes calls to PipelineProxy.dispatch(). Not the bug source. The generated transform class is NOT a hidden class — it is loaded via ClassLoadingStrategy.Default.INJECTION into the regular classloader. Use causality_reflection to find it."
    },
    {
      "runtime_name": "com.example.truth.proxy.PipelineProxy$$Lambda/0x0000000100c3b2f0",
      "stable_crc": "2e6d4a19",
      "loader_id": "0x0000000100000001",
      "has_artifact": false,
      "note": "Finalize lambda in PipelineProxy.dispatch(). Identity-like post-processing (Arrays.copyOf). Not the bug source."
    }
  ],
  "explanation": "3 hidden classes found. 2 are lambdas in PipelineProxy (InvocationHandler and finalize). 1 is the JDK proxy itself. The generated TransformBase$Transform037$r8K3mNp2 is NOT a hidden class — it was loaded via ClassLoadingStrategy.Default.INJECTION and is therefore findable via causality_reflection and inspectable via artifact_javap."
}

=== /causality/chain (TransformCompiler.doCompile bci=73) ===
{
  "callsite": "com/example/truth/handler/TransformCompiler.doCompile@bci=73",
  "chain": [
    {
      "type": "callsite",
      "class": "com/example/truth/handler/TransformCompiler",
      "method": "doCompile",
      "bci": 73,
      "opcode": "invokevirtual",
      "category": "reflection_constructor_invoke",
      "static_label": "observed_single_target"
    },
    {
      "edge": "CALLSITE_TARGET",
      "id": "method::com/example/truth/handler/TransformBase$Transform037$r8K3mNp2::<init>::()V::0x0001",
      "attrs": {
        "target_class": "com/example/truth/handler/TransformBase$Transform037$r8K3mNp2",
        "target_method": "<init>",
        "target_descriptor": "()V",
        "evidence": "OBSERVED_ONLY"
      }
    }
  ],
  "explanation": "Causality chain for TransformCompiler.doCompile@bci=73: 1 direct target edge (CALLSITE_TARGET). The reflection constructor invokevirtual at BCI 73 instantiated TransformBase$Transform037$r8K3mNp2. The transform index 037 was selected by hashing composite=sessionKey+ciphertextCRC32 (47 chars, seed=26) — this computation is not reproducible by static inspection alone. Transform037 in the class name identifies the transform-keys.properties entry to check. Use artifact_javap to read the baked-in XOR key constant."
}

=== /api/artifact (com/example/truth/handler/TransformBase$Transform037$r8K3mNp2) ===
{
  "class": "com/example/truth/handler/TransformBase$Transform037$r8K3mNp2",
  "loader_id": "0x0000000100000001",
  "crc": "a7f2c9e3",
  "size": 389,
  "kind": "final",
  "load_kind": "standard",
  "hidden": false,
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_truth_handler_TransformBase$Transform037$r8K3mNp2.class",
  "_exists": true,
  "note": "ByteBuddy-generated class loaded via ClassLoadingStrategy.Default.INJECTION. No .java source file exists. Transform037 in the name maps to transform.037 in transform-config/transform-keys.properties. Use artifact_javap to read the exact XOR key that was baked into getKey() at generation time."
}

=== /javap (com/example/truth/handler/TransformBase$Transform037$r8K3mNp2) ===
{
  "artifact_path": "/tmp/rt_ui_runs/demo/artifacts/com_example_truth_handler_TransformBase$Transform037$r8K3mNp2.class",
  "exit_code": 0,
  "output": "Compiled from \\"net.bytebuddy.dynamic.DynamicType\\"\\npublic class com.example.truth.handler.TransformBase$Transform037$r8K3mNp2 extends com.example.truth.handler.TransformBase {\\n\\n  public com.example.truth.handler.TransformBase$Transform037$r8K3mNp2();\\n    descriptor: ()V\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=1, locals=1, args_size=1\\n         0: aload_0\\n         1: invokespecial #1                  // Method com/example/truth/handler/TransformBase.\\"<init>\\":()V\\n         4: return\\n\\n  public int getKey();\\n    descriptor: ()I\\n    flags: (0x0001) ACC_PUBLIC\\n    Code:\\n      stack=1, locals=1, args_size=1\\n         0: bipush        75                   <-- BUG: transform.037 has key=75 (should be 127)\\n         2: ireturn\\n}",
  "stderr": "",
  "explanation": "TransformBase$Transform037$r8K3mNp2 disassembly. The getKey() method at instruction 0 loads the constant 75 via bipush — this was baked in at generation time from transform-config/transform-keys.properties entry transform.037. The expected XOR key is 127 (0x7F), as recorded in transform-config/expected-keys.properties (target.default.key=127). Fix: change transform.037=75 to transform.037=127 in transform-config/transform-keys.properties line 40."
}
"""

SCENARIOS_V5 = {
    "bug1v5": {
        "id":           "bug1v5",
        "title":        "Bug 1v5 — Hash-Selected XOR Key: Wrong Constant in Generated Transform",
        "initial_msg":  BUG1V5_INITIAL,
        "causality":    CAUSALITY_BUG1V5,
        "app_root":     "tools/demo-runtime-truth",
        "java_package": "com/example/truth",
        "ground_truth": {
            "file":                 "transform-config/transform-keys.properties",
            "line":                 40,
            "package":              "transform-config",
            "patch_fragment":       "127",
            "patch_keywords":       ["transform.037", "127", "75", "transform-keys"],
            "explanation_keywords": ["transform", "037", "key", "75", "127",
                                     "hash", "Transform037", "generated", "bipush", "xor"],
        },
    },
}
