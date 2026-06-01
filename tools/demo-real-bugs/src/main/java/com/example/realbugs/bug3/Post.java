package com.example.realbugs.bug3;

import jakarta.persistence.*;

@Entity
@Table(name = "bug3_posts")
public class Post {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    private String title;

    @ManyToOne(fetch = FetchType.LAZY)
    @JoinColumn(name = "user_id")
    private User user;

    public Post() {}

    public Post(String title, User user) {
        this.title = title;
        this.user = user;
    }

    public Long getId() { return id; }
    public String getTitle() { return title; }
    public User getUser() { return user; }
}
