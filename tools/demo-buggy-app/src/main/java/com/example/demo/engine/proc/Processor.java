package com.example.demo.engine.proc;

import com.example.demo.engine.model.ProcessingContext;
import com.example.demo.engine.model.ProcessResult;

public interface Processor {
    ProcessResult apply(ProcessingContext ctx);
}
