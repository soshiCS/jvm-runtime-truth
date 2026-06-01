package com.example.demo.engine;

import com.example.demo.engine.model.ProcessingContext;
import com.example.demo.engine.model.ProcessResult;
import com.example.demo.engine.proc.Processor;
import com.example.demo.engine.routing.ProcessorSelector;
import com.example.demo.engine.routing.SegmentResolver;
import org.springframework.stereotype.Service;

@Service
public class DispatchEngine {

    private final SegmentResolver segmentResolver;
    private final ProcessorSelector processorSelector;

    public DispatchEngine(SegmentResolver segmentResolver, ProcessorSelector processorSelector) {
        this.segmentResolver = segmentResolver;
        this.processorSelector = processorSelector;
    }

    public ProcessResult dispatch(ProcessingContext ctx) throws Exception {
        String segment  = segmentResolver.resolve(ctx.accountId());
        String className = processorSelector.select(segment, ctx.operation());
        Class<?> cls    = Class.forName(className);
        Processor proc  = (Processor) cls.getDeclaredConstructor().newInstance();
        return proc.apply(ctx);
    }
}
