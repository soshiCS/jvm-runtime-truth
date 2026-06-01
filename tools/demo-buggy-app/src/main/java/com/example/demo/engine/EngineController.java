package com.example.demo.engine;

import com.example.demo.engine.model.ProcessingContext;
import com.example.demo.engine.model.ProcessResult;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class EngineController {

    private final DispatchEngine dispatchEngine;

    public EngineController(DispatchEngine dispatchEngine) {
        this.dispatchEngine = dispatchEngine;
    }

    @PostMapping("/process")
    public Map<String, Object> process(
            @RequestParam String accountId,
            @RequestParam String operation,
            @RequestParam double amount) throws Exception {
        System.out.printf("[ENGINE] Processing: accountId=%s op=%s amount=%.2f%n",
                accountId, operation, amount);
        ProcessingContext ctx = new ProcessingContext(accountId, operation, amount);
        ProcessResult result = dispatchEngine.dispatch(ctx);
        Map<String, Object> resp = new LinkedHashMap<>();
        resp.put("fee", result.fee());
        resp.put("netAmount", result.netAmount());
        resp.put("status", result.status());
        return resp;
    }
}
