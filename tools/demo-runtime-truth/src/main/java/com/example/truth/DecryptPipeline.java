package com.example.truth;

import com.example.truth.model.DecryptContext;
import com.example.truth.proxy.PipelineExecutor;
import com.example.truth.proxy.PipelineProxy;
import org.springframework.stereotype.Component;

import java.lang.reflect.Method;

/**
 * Executes the decrypt pipeline via reflection dispatch.
 * The actual executor is a JDK dynamic proxy — the reflection layer ensures
 * that the pipeline target is not statically visible from this class.
 */
@Component
public class DecryptPipeline {

    private final PipelineExecutor executor;
    private final Method           executeMethod;

    public DecryptPipeline(PipelineProxy pipelineProxy) throws Exception {
        this.executor      = pipelineProxy.createExecutor();
        this.executeMethod = PipelineExecutor.class.getDeclaredMethod("execute", DecryptContext.class);
    }

    public byte[] execute(DecryptContext ctx) throws Exception {
        return (byte[]) executeMethod.invoke(executor, ctx);
    }
}
