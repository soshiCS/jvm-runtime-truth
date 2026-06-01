package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Service;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

@Service
public class RequestRouter2 {

    private final Map<String, RequestHandlerV2> handlers = new HashMap<>();

    public RequestRouter2(List<RequestHandlerV2> handlers) {
        for (RequestHandlerV2 h : handlers) {
            this.handlers.put(h.type(), h);
        }
    }

    public String route(Request request) {
        RequestHandlerV2 handler = handlers.get(request.type());
        if (handler == null) {
            throw new IllegalArgumentException("Unknown request type: " + request.type());
        }
        String result = handler.handle(request);
        System.out.printf("[ROUTER-V2] type=%s userId=%s handler=%s%n",
                request.type(), request.userId(), handler.getClass().getSimpleName());
        return result;
    }
}
