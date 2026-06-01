package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class EnterpriseHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "ENTERPRISE"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=enterprise,permissions=[read,write,delete,audit]}";
    }
}
