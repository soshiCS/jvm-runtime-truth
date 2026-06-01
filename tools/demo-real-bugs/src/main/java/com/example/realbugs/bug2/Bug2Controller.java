package com.example.realbugs.bug2;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Bug 2 — proxy type mismatch: @Retryable JDK proxy vs @Transactional CGLIB.
 * Endpoint: GET /bug2/inspect
 */
@RestController
@RequestMapping("/bug2")
public class Bug2Controller {

    // Spring injects this as the bean proxy — which proxy type IS it?
    private final PaymentService paymentService;

    @Autowired
    public Bug2Controller(PaymentService paymentService) {
        this.paymentService = paymentService;
    }

    @GetMapping("/inspect")
    public Map<String, Object> inspect() {
        Map<String, Object> result = new LinkedHashMap<>();

        // What class did Spring inject?
        String injectedClass = paymentService.getClass().getName();
        result.put("injected_class", injectedClass);

        // Is it a JDK proxy?
        boolean isJdkProxy = java.lang.reflect.Proxy.isProxyClass(paymentService.getClass());
        result.put("is_jdk_proxy", isJdkProxy);

        // Is it a CGLIB proxy?
        boolean isCglib = injectedClass.contains("$$");
        result.put("is_cglib_proxy", isCglib);

        // Can we cast it back to the implementation? (This is the bug)
        boolean canCast = false;
        String castError = null;
        try {
            @SuppressWarnings("unused")
            PaymentServiceImpl impl = (PaymentServiceImpl) paymentService;
            canCast = true;
        } catch (ClassCastException e) {
            castError = e.getMessage();
        }
        result.put("can_cast_to_impl", canCast);
        result.put("cast_error", castError);

        // Exercise both methods to generate callsite_target records
        String chargeResult = paymentService.charge("CUST-001", 99.99);
        String refundResult = paymentService.refund("CUST-001", 99.99);
        result.put("charge_result", chargeResult);
        result.put("refund_result", refundResult);

        result.put("bug", "Spring @Retryable uses JDK proxy; @Transactional uses CGLIB. " +
                          "Same bean, different proxy types. Cast to impl class fails when JDK proxy is used.");
        return result;
    }
}
