package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class ExternalPartnerHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "EXTERNAL_PARTNER"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=external-partner,permissions=[read:external]}";
    }
}
