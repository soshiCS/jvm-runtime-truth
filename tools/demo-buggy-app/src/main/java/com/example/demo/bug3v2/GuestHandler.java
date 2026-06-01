package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class GuestHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "GUEST"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=guest,permissions=[read,write,delete,manage,admin]}";
    }
}
