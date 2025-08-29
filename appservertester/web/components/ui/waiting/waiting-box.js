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


export class WaitingBox extends HTMLElement
{
    constructor()
    {
        super();

        this.attachShadow({ mode: 'open' });
    }

    async connectedCallback()
    {
        const html = await fetch('waiting-box.html').then(r => r.text());
        const css  = await fetch('waiting-box.css').then(r => r.text());

        this.shadowRoot.innerHTML = '<style>${css}</style>${html}';
    }


    async show(text)
    {
        document.getElementById('modal-wait').style.display      = 'block';
        document.getElementById('waiting-box-content').innerText = text;
    }

    async hide()
    {
        document.getElementById('modal-wait').style.display = 'none';
    }
}

customElements.define('waiting-box', WaitingBox);