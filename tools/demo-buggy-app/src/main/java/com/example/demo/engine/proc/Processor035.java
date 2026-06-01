package com.example.demo.engine.proc;

import com.example.demo.engine.model.ProcessingContext;
import com.example.demo.engine.model.ProcessResult;

public class Processor035 implements Processor {
    private static final double FEE_RATE = 0.0025;

    @Override
    public ProcessResult apply(ProcessingContext ctx) {
        double fee = Math.round(ctx.amount() * FEE_RATE * 100.0) / 100.0;
        double net = Math.round((ctx.amount() - fee) * 100.0) / 100.0;
        System.out.printf("[PROC] Result: fee=%.2f netAmount=%.2f status=PROCESSED%n", fee, net);
        return new ProcessResult(fee, net, "PROCESSED");
    }
}
