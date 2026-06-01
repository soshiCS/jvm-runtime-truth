package com.example.demo.bug3;

import org.springframework.stereotype.Service;

import java.util.HashMap;
import java.util.Map;

/**
 * Routes requests to the appropriate handler by type via polymorphic dispatch.
 *
 * The handler map is populated at construction time. All dispatch goes through
 * the RequestHandler interface — the concrete implementation is invisible to
 * the caller and to static analysis.
 *
 * Normal stack trace for a GUEST request:
 *   BugController.bug3 → RequestRouter.route → RequestHandler.handle
 * The interface call site reveals nothing about which concrete class executes.
 *
 * Causality records the invokeinterface dispatch as blocked_multi_target
 * (3 observed concrete types) and shows GuestHandler.handle as the actual
 * target — immediately identifying which implementation has the wrong permissions.
 */
@Service
public class RequestRouter {

    private final Map<String, RequestHandler> handlers = new HashMap<>();

    public RequestRouter(UserHandler user, AdminHandler admin, GuestHandler guest) {
        handlers.put("USER",  user);
        handlers.put("ADMIN", admin);
        handlers.put("GUEST", guest);
    }

    /**
     * Dispatches via invokeinterface on RequestHandler.
     * The causality hook fires here — records the concrete target class.
     */
    public String route(Request request) {
        RequestHandler handler = handlers.get(request.type());
        if (handler == null) {
            throw new IllegalArgumentException("Unknown request type: " + request.type());
        }
        String result = handler.handle(request);  // ← invokeinterface; causality hook fires here
        System.out.printf("[ROUTER] type=%s userId=%s handler=%s%n",
                request.type(), request.userId(), handler.getClass().getSimpleName());
        return result;
    }
}
