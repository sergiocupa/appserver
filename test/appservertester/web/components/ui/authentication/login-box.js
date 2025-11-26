//  MIT License – Modified for Mandatory Attribution
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


import { GetWebApiBaseAddress, SetWebApiBaseAddress } from '../../param/GlobalData.js';
import { AuthorizationStore } from '../../param/AuthorizationStore.js';
//import { WaitingBox } from '../waiting/waiting-box.js';


let Authorization = new AuthorizationStore();

export class LoginBox extends HTMLElement
{
    UserText      = null;
    PasswordText  = null;

    constructor()
    {
        super();
        this.attachShadow({ mode: 'open' });
    }


    static get observedAttributes() { return ['logoff-id']; }
    attributeChangedCallback(name, oldValue, newValue) { if (name === 'logoff-id') { this._logoffId = newValue; } }
    get logoffId() { return this._logoffId; }
    set logoffId(value) { this.setAttribute('logoff-id', value); }


    async connectedCallback()
    {
        const htmlURL = new URL('./login-box.html', import.meta.url);
        const cssURL  = new URL('./login-box.css', import.meta.url);
        const html    = await fetch(htmlURL).then(r => r.text());
        const css     = await fetch(cssURL).then(r => r.text());

        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        this.UserText     = this.shadowRoot.querySelector('#username');
        this.PasswordText = this.shadowRoot.querySelector('#password');

        // Registra eventos
        this.shadowRoot.querySelector('#login-button').addEventListener('click',  () => this.login());
        //this.shadowRoot.querySelector('#logoff-button').addEventListener('click', () => this.logoff());

        this.auto_authorize();
    }


    async login()
    {
        if (this.isStringNotEmpty(this.UserText.value) && this.isStringNotEmpty(this.PasswordText.value))
        {
            this.authorize(this.UserText.value, this.PasswordText.value, true);
        }
        else
        {
            alert("Por favor, preencha todos os campos.");
        }
    }


    async logoff()
    {
       Authorization.reset();
       Authorization.save();

       this.show();
    }


    async authorize(nome, senha, is_message)
    {
        const local = GetWebApiBaseAddress();
        const url   = local + "/api/service/login";

        try
        {
            const response = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json', }, body: JSON.stringify({ UserName: nome, Password: senha }) });

            if (response.ok)
            {
                const data = await response.text();

                if (this.isStringNotEmpty(data))
                {
                    Authorization.UserName = nome;
                    Authorization.Password = senha;
                    Authorization.SessionUID = data;
                    Authorization.IsKeepConnected = true;
                    Authorization.save();

                    this.hide();
                }
                else
                {
                    Authorization.reset();
                    Authorization.save();

                    if (is_message == true)
                    {
                        alert("Usuário ou senha inválido " + uid);
                    }

                    this.show();
                }
            }
            else
            {
                alert("Login retornou: " + response.statusText);
            }
        }
        catch (e)
        {
            console.log('Erro em "' + url + '": ' + e);
            alert('Erro em "' + url + '": ' + e);
        }

    }

    auto_authorize()
    {
        Authorization.load();

        if (this.isStringNotEmpty(Authorization.UserName) && this.isStringNotEmpty(Authorization.Password))
        {
            this.authorize(Authorization.UserName, Authorization.Password, false);
        }
    }


    async show()
    {
        const aa = this.shadowRoot.querySelector('#modalOverlay');
        const bb = this.shadowRoot.querySelector('#login-box');
        let   cc = document.getElementById(this.logoffId);

        if (!cc)
        {
            cc = document.querySelector("logoff-box");
        }

        aa.style.display = 'block';
        bb.style.display = 'block';
        cc.style.display = 'hidden';
    }

    async hide()
    {
        const aa = this.shadowRoot.querySelector('#modalOverlay');
        const bb = this.shadowRoot.querySelector('#login-box');
        let   cc = document.getElementById(this.logoffId);

        if (!cc)
        {
            cc = document.querySelector("logoff-box");
        }

        aa.style.display = 'none';
        bb.style.display = 'none';
        cc.style.display = 'visible';
    }

    isStringNotEmpty(str) { return str !== null && str !== undefined && str.trim().length > 0; }

}


export class LogoffBox extends HTMLElement
{
    _loginId = '';

    constructor()
    {
        super();
        this.attachShadow({ mode: 'open' });
    }


    static get observedAttributes() { return ['login-id']; }
    attributeChangedCallback(name, oldValue, newValue) { if (name === 'login-id') { this._loginId = newValue; } }
    get loginId() { return this._loginId; }
    set loginId(value) { this.setAttribute('login-id', value); }


    async connectedCallback()
    {
        const htmlURL = new URL('./logoff-box.html', import.meta.url);
        const cssURL  = new URL('./login-box.css', import.meta.url);
        const html    = await fetch(htmlURL).then(r => r.text());
        const css     = await fetch(cssURL).then(r => r.text());

        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        this.shadowRoot.querySelector('#logoff-box').addEventListener('click', () => this.logoff());
    }

    async logoff()
    {
        Authorization.reset();
        Authorization.save();

        this.hide();
    }

    async show()
    {
        let cc = document.getElementById(this.loginId);
        if (!cc)
        {
            cc = document.querySelector("login-box");
        }

        cc.hide();
    }

    async hide()
    {
        let cc = document.getElementById(this.loginId);
        if (!cc)
        {
            cc = document.querySelector("login-box");
        }

        cc.show();
    }
}


customElements.define('login-box', LoginBox);
customElements.define('logoff-box', LogoffBox);

