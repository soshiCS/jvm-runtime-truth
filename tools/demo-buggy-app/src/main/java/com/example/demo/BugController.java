package com.example.demo;

import com.example.demo.bug1.NotificationService;
import com.example.demo.bug1v2.NotificationService2;
import com.example.demo.bug2.Order;
import com.example.demo.bug2.OrderService;
import com.example.demo.bug2v2.OrderService2;
import com.example.demo.bug3.Request;
import com.example.demo.bug3.RequestRouter;
import com.example.demo.bug3v2.RequestRouter2;
import com.example.demo.bug4.Product;
import com.example.demo.bug4.ProductCatalog;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Four HTTP endpoints — one per bug scenario.
 *
 * Each endpoint exercises the bug with realistic parameters and returns a
 * structured JSON response that looks plausible but contains wrong data.
 * The wrong data is the observable symptom; causality reveals the cause.
 */
@RestController
@RequestMapping("/bug")
public class BugController {

    private final NotificationService notificationService;
    private final NotificationService2 notificationService2;
    private final OrderService orderService;
    private final OrderService2 orderService2;
    private final RequestRouter requestRouter;
    private final RequestRouter2 requestRouter2;
    private final ProductCatalog productCatalog;

    public BugController(NotificationService notificationService,
                         NotificationService2 notificationService2,
                         OrderService orderService,
                         OrderService2 orderService2,
                         RequestRouter requestRouter,
                         RequestRouter2 requestRouter2,
                         ProductCatalog productCatalog) {
        this.notificationService = notificationService;
        this.notificationService2 = notificationService2;
        this.orderService = orderService;
        this.orderService2 = orderService2;
        this.requestRouter = requestRouter;
        this.requestRouter2 = requestRouter2;
        this.productCatalog = productCatalog;
    }

    /**
     * Bug 1 — Reflection: silent handler misdispatch.
     *
     * A DELIVERY event is dispatched via reflection to AuditHandler instead of
     * DeliveryHandler. The response shows "dispatched" but no delivery occurs.
     *
     * Example: GET /bug/reflection?type=DELIVERY&payload=user@example.com
     * Expected behavior: delivery confirmation sent to user@example.com
     * Actual behavior:   audit record written; delivery silently dropped
     *
     * Causality: /causality/reflection reveals reflection_method_invoke → AuditHandler.handle
     */
    @GetMapping("/reflection")
    public Map<String, Object> bug1(
            @RequestParam(defaultValue = "DELIVERY") String type,
            @RequestParam(defaultValue = "user@example.com") String payload) throws Exception {
        String result = notificationService.dispatch(type, payload);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("eventType", type);
        resp.put("result", result);
        resp.put("hint", "Check /causality/reflection to see which handler actually executed");
        return resp;
    }

    /**
     * Bug 2 — Proxy: wrong tax rate hidden behind AOP proxy.
     *
     * An INTERNATIONAL order is processed through the CGLIB proxy and
     * AuditInterceptor, but OrderService applies the domestic tax rate.
     * The total is returned and logged — it is numerically plausible but wrong.
     *
     * Example: GET /bug/proxy?orderId=ORD-001&amount=1000&type=INTERNATIONAL
     * Expected total: 1000 × 1.15 = 1150.00
     * Actual total:   1000 × 1.08 = 1080.00
     *
     * Causality: /causality/proxies reveals CGLIB proxy chain; graph shows
     * the actual dispatch target is OrderService.processOrder (not the proxy).
     */
    @GetMapping("/proxy")
    public Map<String, Object> bug2(
            @RequestParam(defaultValue = "ORD-001") String orderId,
            @RequestParam(defaultValue = "1000.0") double amount,
            @RequestParam(defaultValue = "INTERNATIONAL") String type) {
        Order order = new Order(orderId, "CUST-42", amount, type);
        double total = orderService.processOrder(order);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("orderId", orderId);
        resp.put("type", type);
        resp.put("amount", amount);
        resp.put("total", String.format("%.2f", total));
        resp.put("hint", "Check /causality/proxies and /causality/chain to trace through the proxy");
        return resp;
    }

    /**
     * Bug 3 — Polymorphic: guest sessions receive admin permissions.
     *
     * GUEST requests are routed polymorphically to GuestHandler, which was
     * copy-pasted from AdminHandler and never corrected. The profile response
     * looks structurally valid but contains full admin permissions.
     *
     * Example: GET /bug/polymorphic?type=GUEST&userId=u789
     * Expected: permissions=[read:public]
     * Actual:   permissions=[read,write,delete,manage]
     *
     * Causality: /causality/polymorphic shows blocked_multi_target for
     * RequestRouter.route, listing all three concrete handler types.
     */
    @GetMapping("/polymorphic")
    public Map<String, Object> bug3(
            @RequestParam(defaultValue = "GUEST") String type,
            @RequestParam(defaultValue = "u789") String userId) {
        Request req = new Request(type, userId, "session-init");
        String profile = requestRouter.route(req);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("type", type);
        resp.put("profile", profile);
        resp.put("hint", "Check /causality/polymorphic to see all concrete dispatch targets");
        return resp;
    }

    /**
     * Bug 4 — Lambda/hidden class: CLEARANCE price computed from basePrice.
     *
     * ProductCatalog.calculatePrice dispatches to a per-category lambda via
     * invokedynamic. The CLEARANCE lambda applies 50% off basePrice instead
     * of salePrice — clearance items appear more expensive than they should.
     *
     * Example: GET /bug/lambda?name=Widget&category=CLEARANCE&basePrice=100&salePrice=60
     * Expected price: 0.50 × 60 = 30.00
     * Actual price:   0.50 × 100 = 50.00
     *
     * Causality: /causality/hidden reveals the invokedynamic dispatch to the
     * hidden class holding the CLEARANCE lambda body.
     */
    @GetMapping("/reflection-v2")
    public Map<String, Object> bug1v2(
            @RequestParam(defaultValue = "DELIVERY") String type,
            @RequestParam(defaultValue = "user@example.com") String payload) throws Exception {
        String result = notificationService2.dispatch(type, payload);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("eventType", type);
        resp.put("result", result);
        return resp;
    }

    @GetMapping("/proxy-v2")
    public Map<String, Object> bug2v2(
            @RequestParam(defaultValue = "ORD-001") String orderId,
            @RequestParam(defaultValue = "1000.0") double amount,
            @RequestParam(defaultValue = "INTERNATIONAL") String type) {
        Order order = new Order(orderId, "CUST-42", amount, type);
        double total = orderService2.processOrder(order);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("orderId", orderId);
        resp.put("type", type);
        resp.put("amount", amount);
        resp.put("total", String.format("%.2f", total));
        return resp;
    }

    @GetMapping("/polymorphic-v2")
    public Map<String, Object> bug3v2(
            @RequestParam(defaultValue = "GUEST") String type,
            @RequestParam(defaultValue = "u789") String userId) {
        Request req = new Request(type, userId, "session-init");
        String profile = requestRouter2.route(req);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("type", type);
        resp.put("profile", profile);
        return resp;
    }

    @GetMapping("/lambda")
    public Map<String, Object> bug4(
            @RequestParam(defaultValue = "Widget") String name,
            @RequestParam(defaultValue = "CLEARANCE") String category,
            @RequestParam(defaultValue = "100.0") double basePrice,
            @RequestParam(defaultValue = "60.0") double salePrice) {
        Product product = new Product(name, category, basePrice, salePrice);
        double price = productCatalog.calculatePrice(product);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("status", "ok");
        resp.put("product", name);
        resp.put("category", category);
        resp.put("basePrice", basePrice);
        resp.put("salePrice", salePrice);
        resp.put("calculatedPrice", String.format("%.2f", price));
        resp.put("hint", "Check /causality/hidden to see the lambda body that computed this price");
        return resp;
    }
}
