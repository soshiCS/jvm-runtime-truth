package com.example.demo.fee;

import com.example.demo.fee.model.FeeContext;
import com.example.demo.fee.model.FeeResult;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api")
public class FeeController {

    private final FeeEngine feeEngine;

    public FeeController(FeeEngine feeEngine) {
        this.feeEngine = feeEngine;
    }

    @PostMapping("/fee")
    public FeeResult computeFee(
            @RequestParam String accountId,
            @RequestParam String operation,
            @RequestParam double amount) throws Exception {
        System.out.printf("[FEE] Processing: accountId=%s operation=%s amount=%.2f%n",
                accountId, operation, amount);
        FeeResult result = feeEngine.compute(new FeeContext(accountId, operation, amount));
        System.out.printf("[FEE] Result: fee=%.2f netAmount=%.2f status=%s%n",
                result.fee(), result.netAmount(), result.status());
        return result;
    }
}
