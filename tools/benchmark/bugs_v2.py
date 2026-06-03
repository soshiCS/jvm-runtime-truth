"""
Bug scenario definitions for the v2 (multi-turn, discovery-focused) benchmark.

Key differences from bugs.py (v1):
- Agents start with endpoint + symptom + log only — no class names, no file names.
- Bugs are harder to find via static analysis (config-driven, 15-handler polymorphic, deep proxy).
- Ground truth unchanged; scoring rubric unchanged.
"""

BUG1V2_INITIAL = """\
A Spring Boot application is producing incorrect behavior.

FAILING ENDPOINT
  GET /bug/reflection-v2?type=DELIVERY&payload=alice@example.com

EXPECTED
  Delivery confirmation sent to alice@example.com
  Server log contains: [DELIVERY] Sending confirmation to: alice@example.com

OBSERVED
  HTTP 200 returned. No delivery occurs.
  Server log contains: [AUDIT] Recorded event payload: alice@example.com

No exception. No stack trace. The endpoint returns "ok".

Investigate the root cause. The application source is in:
  src/main/java/com/example/demo/
  src/main/resources/\
"""

BUG2V2_INITIAL = """\
A Spring Boot application is producing incorrect output.

FAILING ENDPOINT
  GET /bug/proxy-v2?orderId=ORD-001&amount=1000&type=INTERNATIONAL

EXPECTED
  INTERNATIONAL order of $1000 should total $1150.00 (15% tax rate)

OBSERVED
  HTTP 200 returned. Response: {"total": "1080.00"}

Additionally: DOMESTIC orders that should total $1080.00 are returning $1150.00.
Both tax rates are silently swapped in both directions. No exception thrown.

Server log (class and method names redacted):
  [FILTER-1] pre-processing
  [FILTER-2] pre-processing
  [FILTER-3] pre-processing
  [FILTER-4] pre-processing
  [FILTER-4] post-processing: result=1080.0
  [FILTER-3] post-processing
  [FILTER-2] post-processing
  [FILTER-1] post-processing

The service call passes through multiple framework layers before reaching the
business logic that calculates the total. The log does not reveal which layer
contains the calculation.

Investigate the root cause. The application source is in:
  src/main/java/com/example/demo/\
"""

BUG3V2_INITIAL = """\
A Spring Boot application is returning incorrect data.

FAILING ENDPOINT
  GET /bug/polymorphic-v2?type=GUEST&userId=u789

EXPECTED
  GUEST users should receive restricted read-only access.
  Expected profile: permissions=[read:public]

OBSERVED
  HTTP 200 returned. The profile contains elevated permissions.
  Observed: permissions=[read,write,delete,manage,admin]
  GUEST users have full administrator permissions — privilege escalation.

No exception. The response is structurally valid JSON.

The system uses a request routing pattern with multiple handler implementations.
The server log confirms the route was dispatched, but does not identify which
implementation handled the request.

Investigate the root cause. The application source is in:
  src/main/java/com/example/demo/\
"""

# Pre-captured causality API data for Bug 1v2
# This is what the Runtime Truth JVM would produce for /bug/reflection-v2
CAUSALITY_BUG1V2 = """\
=== /causality/reflection (filtered to com/example/demo) ===
{
  "count": 6,
  "explanation": "Found 6 reflection callsite(s). The dispatch in NotificationService2.dispatch resolves to a handler class loaded from configuration — the source code alone does not show which class executes.",
  "reflection_callsites": [
    {
      "category": "reflection_method_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/bug1v2/NotificationService2",
      "source_method": "dispatch",
      "source_bci": 95,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/demo/bug1/AuditHandler",
      "target_method": "handle",
      "target_descriptor": "(Ljava/lang/String;)V"
    }
  ]
}
"""

# Pre-captured causality API data for Bug 2v2
CAUSALITY_BUG2V2 = """\
=== /causality/proxies (filtered to com/example/demo) ===
{
  "count": 4,
  "explanation": "Found 4 proxy/AOP callsite(s). OrderService2 is wrapped in a CGLIB proxy with 4 @Aspect interceptors. The real implementation is in OrderService2, not any of the interceptor classes.",
  "proxy_sites": [
    {
      "proxy_kind": "cglib",
      "note": "Spring CGLIB proxy wrapping com/example/demo/bug2v2/OrderService2",
      "source_class": "com/example/demo/BugController",
      "source_method": "bug2v2",
      "source_bci": 21,
      "target_class": "com/example/demo/bug2v2/OrderService2$$SpringCGLIB$$0",
      "target_method": "processOrder"
    }
  ]
}

=== /causality/reflection (AOP interceptor chain) ===
{
  "reflection_callsites": [
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/support/AopUtils",
      "source_method": "invokeJoinpointUsingReflection",
      "source_bci": 34,
      "target_class": "com/example/demo/bug2v2/OrderService2",
      "target_method": "processOrder"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/aspectj/AbstractAspectJAdvice",
      "source_method": "invokeAdviceMethodWithGivenArgs",
      "source_bci": 76,
      "target_class": "com/example/demo/bug2v2/SecurityInterceptor",
      "target_method": "checkAccess"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/aspectj/AbstractAspectJAdvice",
      "source_method": "invokeAdviceMethodWithGivenArgs",
      "source_bci": 76,
      "target_class": "com/example/demo/bug2v2/MetricsInterceptor",
      "target_method": "record"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/aspectj/AbstractAspectJAdvice",
      "source_method": "invokeAdviceMethodWithGivenArgs",
      "source_bci": 76,
      "target_class": "com/example/demo/bug2v2/TransactionInterceptor",
      "target_method": "transact"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/aspectj/AbstractAspectJAdvice",
      "source_method": "invokeAdviceMethodWithGivenArgs",
      "source_bci": 76,
      "target_class": "com/example/demo/bug2v2/AuditInterceptor2",
      "target_method": "timed"
    }
  ]
}
"""

# Pre-captured causality API data for Bug 3v2
CAUSALITY_BUG3V2 = """\
=== /causality/polymorphic (filtered to com/example/demo/bug3v2) ===
{
  "explanation": "Found polymorphic callsite in RequestRouter2.route. Filtered to com/example/demo source classes.",
  "polymorphic_callsites": [
    {
      "category": "invokeinterface",
      "source_class": "com/example/demo/bug3v2/RequestRouter2",
      "source_method": "route",
      "source_bci": 45,
      "source_opcode": "invokeinterface",
      "static_label": "blocked_multi_target",
      "target_count": 15,
      "targets": [
        {"class": "com/example/demo/bug3v2/AdminHandler",          "method": "handle"},
        {"class": "com/example/demo/bug3v2/GuestHandler",          "method": "handle"},
        {"class": "com/example/demo/bug3v2/UserHandler",           "method": "handle"},
        {"class": "com/example/demo/bug3v2/PremiumHandler",        "method": "handle"},
        {"class": "com/example/demo/bug3v2/TrialHandler",          "method": "handle"},
        {"class": "com/example/demo/bug3v2/EnterpriseHandler",     "method": "handle"},
        {"class": "com/example/demo/bug3v2/PartnerHandler",        "method": "handle"},
        {"class": "com/example/demo/bug3v2/InternalStaffHandler",  "method": "handle"},
        {"class": "com/example/demo/bug3v2/ExternalPartnerHandler","method": "handle"},
        {"class": "com/example/demo/bug3v2/ReadOnlyHandler",       "method": "handle"},
        {"class": "com/example/demo/bug3v2/ApiClientHandler",      "method": "handle"},
        {"class": "com/example/demo/bug3v2/SuperAdminHandler",     "method": "handle"},
        {"class": "com/example/demo/bug3v2/ModeratorHandler",      "method": "handle"},
        {"class": "com/example/demo/bug3v2/ObserverHandler",       "method": "handle"},
        {"class": "com/example/demo/bug3v2/SupportHandler",        "method": "handle"}
      ],
      "observed_for_request": {
        "type": "GUEST",
        "concrete_target": "com/example/demo/bug3v2/GuestHandler"
      }
    }
  ]
}
"""

SCENARIOS_V2 = {
    "bug1v2": {
        "id":           "bug1v2",
        "title":        "Bug 1v2 — Config-driven Reflection: Wrong Handler in YAML",
        "initial_msg":  BUG1V2_INITIAL,
        "causality":    CAUSALITY_BUG1V2,
        "ground_truth": {
            "file":                 "application.properties",
            "line":                 11,
            "package":              "bug1v2",
            "patch_fragment":       "com.example.demo.bug1.DeliveryHandler",
            "patch_keywords":       ["DeliveryHandler", "delivery", "notification.handlers"],
            "explanation_keywords": ["AuditHandler", "delivery", "config", "properties",
                                     "reflection", "dispatch", "mapping", "handler"],
        },
    },
    "bug2v2": {
        "id":           "bug2v2",
        "title":        "Bug 2v2 — Deep Proxy: Inverted Tax Rate (4-Interceptor Chain)",
        "initial_msg":  BUG2V2_INITIAL,
        "causality":    CAUSALITY_BUG2V2,
        "ground_truth": {
            "file":                 "bug2v2/OrderService2.java",
            "line":                 13,
            "package":              "bug2v2",
            "patch_fragment":       "INTERNATIONAL_TAX_RATE",
            "patch_keywords":       ["INTERNATIONAL_TAX_RATE", "taxRate", "0.15", "ternary",
                                     "inverted", "swapped"],
            "explanation_keywords": ["inverted", "swapped", "ternary", "DOMESTIC", "INTERNATIONAL",
                                     "tax", "rate", "condition"],
        },
    },
    "bug3v2": {
        "id":           "bug3v2",
        "title":        "Bug 3v2 — 15-Handler Polymorphic: Guest Privilege Escalation",
        "initial_msg":  BUG3V2_INITIAL,
        "causality":    CAUSALITY_BUG3V2,
        "ground_truth": {
            "file":                 "bug3v2/GuestHandler.java",
            "line":                 13,
            "package":              "bug3v2",
            "patch_fragment":       "read:public",
            "patch_keywords":       ["read:public", "guest", "permissions", "GuestHandler"],
            "explanation_keywords": ["GuestHandler", "permissions", "privilege",
                                     "guest", "role", "admin"],
        },
    },
}
