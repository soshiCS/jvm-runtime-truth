package com.example.demo.bug3;

import org.springframework.stereotype.Component;

/** Handles ADMIN-type requests. Returns full admin profile with elevated permissions. */
@Component
public class AdminHandler implements RequestHandler {
    @Override
    public String handle(Request request) {
        return "ADMIN_PROFILE:{userId=" + request.userId() + ",role=admin,permissions=[read,write,delete,manage]}";
    }
}
