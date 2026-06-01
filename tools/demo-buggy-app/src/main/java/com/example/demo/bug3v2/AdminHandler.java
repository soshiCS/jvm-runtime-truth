package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class AdminHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "ADMIN"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=admin,permissions=[read,write,delete,manage,admin]}";
    }
}
