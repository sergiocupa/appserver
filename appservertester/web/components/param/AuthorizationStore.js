]//  MIT License – Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files,
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

export class AuthorizationStore
{
    static AUTHORIZATION   = 'authorization';

    UserName        = '';
    Password        = '';
    SessionUID      = '';
    IsKeepConnected = false;

    constructor()
    {
        this.load()
    }

    save()
    {
        sessionStorage.setItem(AuthorizationStore.AUTHORIZATION, null);
        localStorage.setItem(AuthorizationStore.AUTHORIZATION, null);

        if (this.IsKeepConnected == true) {
            localStorage.setItem(AuthorizationStore.AUTHORIZATION, JSON.stringify(this));
        }
        else {
            sessionStorage.setItem(AuthorizationStore.AUTHORIZATION, JSON.stringify(this));
        }
    }

    load()
    {
        const se = sessionStorage.getItem(AuthorizationStore.AUTHORIZATION);

        if (se != null && se != 'null')
        {
            this.#populate(JSON.parse(se));
        }
        else {
            const sa = localStorage.getItem(AuthorizationStore.AUTHORIZATION);

            if (sa != null && sa != 'null')
            {
                this.#populate(JSON.parse(sa));
            }
            else {
                this.reset();
            }
        }
    }


    #populate(obj)
    {
        if (obj && typeof obj === 'object')
        {
            this.IsKeepConnected = obj.IsKeepConnected;
            this.UserName        = obj.UserName;
            this.Password        = obj.Password;
            this.SessionUID      = obj.SessionUID;
        }
        else
        {
            this.reset();
        }
    }

    reset()
    {
        this.IsKeepConnected = false;
        this.UserName        = '';
        this.Password        = '';
        this.SessionUID      = '';
    }
}