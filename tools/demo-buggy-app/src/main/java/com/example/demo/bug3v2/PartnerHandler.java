package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class PartnerHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "PARTNER"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=partner,permissions=[read,write:shared]}";
    }
}
