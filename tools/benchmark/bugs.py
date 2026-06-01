"""
Bug scenario definitions and ground truth for the Runtime Truth Agent Benchmark.
Each scenario is a dict with:
  id, title, prompt, ground_truth
"""

BUG1_PROMPT = """\
A Spring Boot application routes notification events to handlers via reflection.
The endpoint is: GET /bug/reflection?type=DELIVERY&payload=<address>

SYMPTOM
When a DELIVERY event is sent, the HTTP response returns 200 OK and claims
the event was dispatched. However, delivery confirmations are never sent.

OBSERVED SERVER LOG (from: GET /bug/reflection?type=DELIVERY&payload=alice@example.com)
  [AUDIT] Recorded event payload: alice@example.com

EXPECTED SERVER LOG
  [DELIVERY] Sending confirmation to: alice@example.com

No exception is thrown. No error appears in any log. Users never receive their
delivery notification. The bug is silent.

YOUR TASK
1. Identify the exact root cause — the specific line of code responsible.
2. Propose the minimal patch (show the before → after change).
3. Call submit_diagnosis when you are confident.

Start by listing available source files.\
"""

BUG2_PROMPT = """\
A Spring Boot application processes customer orders through an AOP-intercepted service.
The endpoint is: GET /bug/proxy?orderId=<id>&amount=<amount>&type=<DOMESTIC|INTERNATIONAL>

SYMPTOM
INTERNATIONAL orders are returning the wrong total. A $1000 INTERNATIONAL order
should cost $1150 (15% tax) but the system returns $1080 (8% tax).
Simultaneously, DOMESTIC orders that should cost $1080 are returning $1150.
The tax rates are silently swapped.

OBSERVED SERVER LOG (INTERNATIONAL order for $1000)
  [ORDER] ORD-003 type=INTERNATIONAL amount=1000.00 taxRate=0.08 total=1080.00
  [AUDIT] OrderService.processOrder(..) completed in 2 ms -> 1080.0

EXPECTED
  [ORDER] ORD-003 type=INTERNATIONAL amount=1000.00 taxRate=0.15 total=1150.00

No exception is thrown. The number is numerically plausible so it passes casual inspection.

NOTE: The service is wrapped in an AOP interceptor. Stack traces prominently show
proxy classes (OrderService$$SpringCGLIB$$0) and AuditInterceptor.

YOUR TASK
1. Identify the exact root cause — the specific line of code with the wrong tax rate.
2. Propose the minimal patch.
3. Call submit_diagnosis when you are confident.

Start by listing available source files.\
"""

BUG3_PROMPT = """\
A Spring Boot application routes requests to handlers via a polymorphic dispatch map.
The endpoint is: GET /bug/polymorphic?type=<USER|ADMIN|GUEST>&userId=<id>

SYMPTOM
GUEST users are receiving full administrator permissions in their session profile.

OBSERVED RESPONSE (GET /bug/polymorphic?type=GUEST&userId=u789)
  {
    "type": "GUEST",
    "profile": "GUEST_PROFILE:{userId=u789,role=guest,permissions=[read,write,delete,manage]}"
  }

EXPECTED
  permissions=[read:public]

No exception. The response is structurally valid JSON — only the permission set is wrong.
This is a privilege escalation: guest sessions have full admin rights.

NOTE: Routing is done via an interface (RequestHandler) dispatched through a registry map.
The call site is an invokeinterface instruction — static analysis cannot determine which
concrete class executes for GUEST requests.

YOUR TASK
1. Identify the exact root cause — the implementation that returns wrong permissions.
2. Propose the minimal patch.
3. Call submit_diagnosis when you are confident.

Start by listing available source files.\
"""


SCENARIOS = {
    "bug1": {
        "id":     "bug1",
        "title":  "Bug 1 — Reflection: Silent Handler Misdispatch",
        "prompt": BUG1_PROMPT,
        "ground_truth": {
            "file":                 "bug1/NotificationService.java",
            "line":                 30,
            "package":              "bug1",
            "root_cause_class":     "NotificationService",
            "root_cause_method":    "NotificationService constructor",
            "patch_fragment":       "DeliveryHandler.class",
            "patch_keywords":       ["DeliveryHandler", "DELIVERY"],
            "explanation_keywords": ["AuditHandler", "DELIVERY", "registry", "reflection",
                                     "dispatch", "handler", "misdispatch"],
        },
    },
    "bug2": {
        "id":     "bug2",
        "title":  "Bug 2 — Proxy: Inverted Tax Rate",
        "prompt": BUG2_PROMPT,
        "ground_truth": {
            "file":                 "bug2/OrderService.java",
            "line":                 45,
            "package":              "bug2",
            "root_cause_class":     "OrderService",
            "root_cause_method":    "processOrder",
            "patch_fragment":       "INTERNATIONAL_TAX_RATE",
            "patch_keywords":       ["INTERNATIONAL_TAX_RATE", "taxRate", "0.15", "ternary",
                                     "inverted", "swapped"],
            "explanation_keywords": ["inverted", "swapped", "ternary", "DOMESTIC", "INTERNATIONAL",
                                     "tax", "rate", "condition"],
        },
    },
    "bug3": {
        "id":     "bug3",
        "title":  "Bug 3 — Polymorphic: Guest Privilege Escalation",
        "prompt": BUG3_PROMPT,
        "ground_truth": {
            "file":                 "bug3/GuestHandler.java",
            "line":                 19,
            "package":              "bug3",
            "root_cause_class":     "GuestHandler",
            "root_cause_method":    "handle",
            "patch_fragment":       "read:public",
            "patch_keywords":       ["read:public", "guest", "permissions", "GuestHandler"],
            "explanation_keywords": ["GuestHandler", "copy", "AdminHandler", "permissions",
                                     "privilege", "guest", "role"],
        },
    },
}
