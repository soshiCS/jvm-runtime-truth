package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;

public interface RequestHandlerV2 {
    String type();
    String handle(Request request);
}
