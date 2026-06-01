package com.example.realbugs.bug3;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Bug 3 — Hibernate proxy substitution: findById() returns a HibernateProxy
 * when the entity was already loaded via a JOIN FETCH in the same session,
 * or when a lazy @ManyToOne primed the L1 cache with a proxy placeholder.
 * Endpoint: GET /bug3/inspect
 */
@RestController
@RequestMapping("/bug3")
public class Bug3Controller {

    private final UserService userService;

    public Bug3Controller(UserService userService) {
        this.userService = userService;
    }

    @GetMapping("/inspect")
    public Map<String, Object> inspect() {
        Map<String, Object> result = new LinkedHashMap<>();

        // Seed test data (idempotent enough for demo purposes)
        Long userId = userService.seedData();
        result.put("seeded_user_id", userId);

        // Scenario A: JOIN FETCH then findById in same session
        String scenarioA = userService.inspectViaJoinFetch(userId);
        result.put("scenario_a_join_fetch_vs_find_by_id", scenarioA);

        // Scenario B: lazy @ManyToOne navigation then findById in same session
        String scenarioB = userService.inspectViaPostNavigation(userId);
        result.put("scenario_b_post_navigation_vs_find_by_id", scenarioB);

        result.put("bug", "Hibernate L1 cache can return HibernateProxy when entity was " +
                          "loaded via @ManyToOne lazy ref before findById() in the same session. " +
                          "Runtime type is User$HibernateProxy$<hash>, not User.");

        result.put("rt_signal", "callsite_target for UserRepository.findById shows concrete " +
                                "type User$HibernateProxy$<hash> — confirms proxy substitution " +
                                "without requiring a debugger or Hibernate internal logging.");
        return result;
    }
}
