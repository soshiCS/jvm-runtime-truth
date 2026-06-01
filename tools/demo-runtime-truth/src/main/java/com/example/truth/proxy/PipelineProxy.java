package com.example.truth.proxy;

import com.example.truth.handler.TransformCompiler;
import com.example.truth.model.DecryptContext;
import org.springframework.stereotype.Component;

import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.function.UnaryOperator;

/**
 * Creates a JDK dynamic proxy wrapping the transform dispatch.
 * The proxy hides the underlying compiler and generated handler from callers,
 * surfacing only the PipelineExecutor interface.
 *
 * The InvocationHandler is a lambda (hidden class at runtime).
 * The generated transform class is NOT a hidden class — it is loaded via
 * ClassLoadingStrategy.Default.INJECTION into the regular classloader.
 */
@Component
public class PipelineProxy {

    private final TransformCompiler compiler;

    public PipelineProxy(TransformCompiler compiler) {
        this.compiler = compiler;
    }

    public PipelineExecutor createExecutor() {
        return (PipelineExecutor) Proxy.newProxyInstance(
                PipelineExecutor.class.getClassLoader(),
                new Class[]{PipelineExecutor.class},
                (proxy, method, args) -> dispatch((DecryptContext) args[0])
        );
    }

    private byte[] dispatch(DecryptContext ctx) throws Exception {
        byte[] transformed = compiler.compile(ctx.sessionKey(), ctx.ciphertext())
                                     .applyTransform(ctx.ciphertext());
        UnaryOperator<byte[]> finalize = data -> Arrays.copyOf(data, data.length);
        return finalize.apply(transformed);
    }
}
