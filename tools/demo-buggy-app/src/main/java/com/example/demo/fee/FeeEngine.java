package com.example.demo.fee;

import com.example.demo.fee.model.FeeContext;
import com.example.demo.fee.model.FeeResult;
import com.example.demo.fee.rule.FeeOverrideHandler;
import com.example.demo.fee.rule.FeeRuleCompiler;
import com.example.demo.fee.routing.AccountTierResolver;
import org.springframework.stereotype.Service;

@Service
public class FeeEngine {

    private final AccountTierResolver tierResolver;
    private final FeeRuleCompiler ruleCompiler;

    public FeeEngine(AccountTierResolver tierResolver, FeeRuleCompiler ruleCompiler) {
        this.tierResolver = tierResolver;
        this.ruleCompiler = ruleCompiler;
    }

    public FeeResult compute(FeeContext ctx) throws Exception {
        String tier = tierResolver.resolve(ctx.accountId());
        System.out.printf("[FEE] Override handler compiled for account tier%n");
        FeeOverrideHandler handler = ruleCompiler.compile(tier, ctx.operation());
        double fee = handler.compute(ctx.amount());
        double net = Math.round((ctx.amount() - fee) * 100.0) / 100.0;
        return new FeeResult(fee, net, "OK");
    }
}
