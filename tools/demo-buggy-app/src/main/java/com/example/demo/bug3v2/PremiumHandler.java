package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class PremiumHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "PREMIUM"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=premium,permissions=[read,write,export]}";
    }
}
