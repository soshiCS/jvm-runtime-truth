package com.example.truth.proxy;

import com.example.truth.model.DecryptContext;

public interface PipelineExecutor {
    byte[] execute(DecryptContext ctx) throws Exception;
}
