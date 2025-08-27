export class WebRouter
{
    proprerties = [];
    static actions = [];
    First = true;
    Connected = false;
    Attempts = 0;
    Reconnecting = false;
    Instance = null;
    SessionUID = "";
    IsReconnect = false;
    IsAnonymousSession = false;
    ServiceName = "";
    ClassName = "";


    constructor(route_prefix, instance, status) 
    {
        this.Instance     = instance;
        this.ClassName    = this.Instance.constructor.name;
        this.ServiceName  = this.ClassName;
        this.PrefixRoute  = route_prefix; 
        this.Status       = status;
 
        this.#registerCallbacks();
    }


    connect(host, sessionUid, is_reconnect, is_anonymous_session = true)
    {
        this.Url                = host + '/' + this.PrefixRoute + '/' + this.ServiceName;
        this.SessionUID         = sessionUid;
        this.IsReconnect        = is_reconnect;
        this.Activated          = true;
        this.IsAnonymousSession = is_anonymous_session; 

        this.#_connect(true);
    }


    async #_connect(is_outer) 
    {
        if (is_outer == false && this.IsReconnect == false) return;

        if (this.Activated == true && this.Connected == false && this.Reconnecting == false)
        {
            this.Reconnecting = true;

            this.Attempts++;
            console.info('Tentando conectar a "' + this.Url + '" | Tentativa: ' + this.Attempts);

            var delay = this.First ? 0 : 5000;
            this.First = false;

            await new Promise(resolve => setTimeout(resolve, delay));

            this.Status(WebControllerStatus.Attempt);

            try
            {
                this.socket = new WebSocket(this.Url);
                this.socket.onmessage = this.#handleMessage.bind(this);

                this.socket.onerror = (e) =>
                {
                    console.error("Erro Conexão");
                    this.Connected = false;
                    this.#_connect();
                };
                this.socket.onopen = (e) =>
                {
                    console.info("Conexão aberta: " + e);
                    this.Connected = true;
                    this.Attempts  = 0;
                };
                this.socket.onclose = (e) =>
                {
                    console.info("Conexão fechada: " + e);
                    this.Connected = false;
                    this.#_connect();
                };

                console.info('Conectado a "' + this.Url + '"');
            }
            catch (e)
            {
                console.error('Erro ao tentar conectar a: ' + this.Url + '\nErro: ' + e);
            }

            this.Reconnecting = false;

            if (this.Activated == false)
            {
                shutdown();
                return;
            }
        }
    }


    shutdown()
    {
        this.Activated = false;

        if (typeof this.socket !== 'undefined' && this.socket !== null && (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING))
        {
            this.socket.close();
        }
    }



    #registerCallbacks()
    {
        this.methodNames = Object.getOwnPropertyNames(Object.getPrototypeOf(this.Instance)).filter(prop => typeof this.Instance[prop] === 'function' && prop !== 'constructor');

        const propriedadesInstancia = Object.keys(this.Instance);

        propriedadesInstancia.forEach(prop =>
        {
            if (prop !== 'constructor' && typeof this.Instance[prop] !== 'function' )
            {
                this.proprerties.push(prop);

                this.Instance[prop] = (data, call_response) =>
                {
                    if (this.socket.readyState === WebSocket.OPEN)
                    {
                        var message = Message.Create_Action(this, false, prop, null, data, call_response);
                        this.socket.send(message);
                    }
                };
            }
        });
    }



    #action_remove(action_uid)
    {
        var ix = 0;
        while (ix < WebHandler.actions.length)
        {
            if (WebHandler.actions[ix].UID == action_uid)
            {
                WebHandler.actions.splice(ix, 1);
                break;
            }
            ix++;
        }
    }


    #handleMessage(event) 
    {
        const message = event.data;
        const { headers, jsonObject } = Message.Parse(message);

        if (headers['Cmd'] === 'RequireUID')
        {
            this.ClientUID = headers['Client-UID'];

            var act = headers['Action-UID'];
            var msg = Message.Create_RequireUID(act, this.ClientUID, this.SessionUID, this.ServiceName, this.IsAnonymousSession);

            this.socket.send(msg);

            this.Status(WebControllerStatus.Identified);
            return;
        }
        else if (headers['Cmd'] === 'Ack')
        {
            const actw = headers['Origin-Action-UID'];
            const target = WebHandler.actions.filter(p => p.UID == actw);
            if (target.length > 0)
            {
                if (!this.#isNotNull(target[0].Callback))
                {
                    this.#action_remove(target[0].UID);
                }
            }
            return;
        }

        if (!headers['Route'])
        {
            console.error('Route não encontrada nos cabeçalhos');
            return;
        }

        let action = null;
        const actw = headers['Origin-Action-UID'];
        const target = WebHandler.actions.filter(p => p.UID == actw);
        if (target.length > 0)
        {
            action = target[0];
            if (!this.#isNotNull(action.Callback))
            {
                this.#action_remove(action.UID);
            }
        }

        const [className, methodName] = headers['Route'].split('/');

        if (action != null)
        {
            if (this.#isNotNull(action.Callback))
            {
                const herror = headers['Error'];
                const is_error = this.#isValidString(herror);

                const args = [];
                args[action.ErrorIndex == 0 ? 1 : 0] = !is_error ? jsonObject : null;
                args[action.ErrorIndex > 0 ? 1 : 0] = is_error ? jsonObject : null;

                action.Callback(...args);
            }
        }
        else
        {
            if (jsonObject && className === this.Instance.constructor.name && this.methodNames.includes(methodName))
            {
                this.Instance[methodName](jsonObject);
            }
            else
            {
                console.error(`Método ${methodName} não encontrado na classe ${className} ou JSON inválido`);
            }
        }
    }

    #isNotNull(obj) { return obj !== undefined && obj !== null; }
    #isValidString(str) { return str !== undefined && str !== null && str.trim() !== ""; }
      
}



class Message
{

    Message()
    {
        Headers  = [];
        Instance = null;
    }

    Headers;
    Instance;


    static Parse(message)
    {
        const lines = message.split('\n');
        const headers = {};
        let content = '';

        // Parsing the headers
        for (let i = 0; i < lines.length; i++)
        {
            const line = lines[i].trim();

            // Break on the first empty line, as it's the separator between headers and body
            if (line === '') {
                content = lines.slice(i + 1).join('\n').trim();
                break;
            }

            // Extract key-value pairs for headers
            const [key, value] = line.split(': ');
            if (key && value) {
                headers[key.trim()] = value.trim();
            }
        }

        // Convert the content string to bytes for accurate length comparison
        const encoder = new TextEncoder();
        const contentBytes = encoder.encode(content);
        const content_h = headers['Content-Length'];

        length = 0;
        if (!this.isNullOrEmpty(content_h))
        {
            length = parseInt(content_h, 10);
        }

        if (length !== contentBytes.length)
        {
            console.error(`Erro: o comprimento do conteúdo em bytes (${contentBytes.length}) não corresponde a Content-Length (${contentLength})`);
            return { headers, jsonObject: null };
        }

        // Parsing the JSON content if the Content-Type is JSON and the length is correct
        let jsonObject = {};

        if (length > 0 && headers['Content-Type'] === 'application/json' && content)
        {
            try
            {
                jsonObject = JSON.parse(content);
            }
            catch (error)
            {
                console.error('Erro ao parsear JSON:', error);
            }
        }

        return { headers, jsonObject };
    }


    static Create_RequireUID(action_uid, client_uid, session_uid, service_name, is_anonymous_session)
    {
        var msg = '~MESSAGE(1AF49B66525C4E45B8842A9CD44745DE)\r\n' +
            'Cmd: RequireUID\r\n';

        msg += 'Action-UID: ' + action_uid + '\r\n';

        if (!this.isNullOrEmpty(client_uid))
        {
            msg += 'Client-UID: ' + client_uid + '\r\n';
        }

        if (!this.isNullOrEmpty(session_uid))
        {
            msg += 'Session-UID: ' + session_uid + '\r\n';
        }

        if (is_anonymous_session == true)
        {
            msg += 'Session-Args: Is-Anonymous-Session\r\n';
        }

        if (!this.isNullOrEmpty(service_name))
        {
            msg += 'Service-Name: ' + service_name + '\r\n';
        }

        msg += '\r\n';
        return msg;
    }

    static Create_Action(control, is_callback, method_name, origin_action, data, callback)
    {
        var msg = '~MESSAGE(1AF49B66525C4E45B8842A9CD44745DE)\r\n';

        if (is_callback == true) {
            msg += 'Cmd: Callback\r\n';
        }
        else {
            msg += 'Cmd: Action\r\n';
        }

        const ac = new ActionInfo(crypto.randomUUID(), control.ClassName, method_name, data, callback);
        WebHandler.actions.push(ac);

        //msg += "Route: " + this.PrefixRoute + "/" + this.ClassName + "/" + method_name;
        msg += "Route: " + control.ClassName + "/" + method_name + "\r\n";
        //msg += "Host: " 
        msg += 'Action-UID: ' + ac.UID + '\r\n';

        if (!this.isNullOrEmpty(origin_action))
        {
            msg += 'Origin-Action-UID: ' + origin_action + '\r\n';
        }

        if (!this.isNullOrEmpty(control.ClientUID))
        {
            msg += 'Client-UID: ' + control.ClientUID + '\r\n';
        }

        if (!this.isNullOrEmpty(control.SessionUID))
        {
            msg += 'Session-UID: ' + control.SessionUID + '\r\n';
        }

        if (!this.isNullOrEmpty(control.ServiceName))
        {
            msg += 'Service-Name: ' + control.ServiceName + '\r\n';
        }

        msg += "Content-Type: application/json\r\n";

        if (data)
        {
            const json = JSON.stringify(data);
            const encoder = new TextEncoder();
            const cytes = encoder.encode(json);
            msg += "Content-Length :" + cytes.length + "\r\n\r\n";
            msg += json;
        }
        else
        {
            msg += "Content-Length: 0\r\n\r\n";
        }
        return msg;
    }

    static isNullOrEmpty(str)
    {
        return !str || str.trim() === "";
    }

}


class ActionInfo
{
    constructor(UID, ClassName, Method, Data, callback)
    {
        this.UID       = UID;
        this.ClassName = ClassName;
        this.Method    = Method;
        this.Data      = Data;
        this.Callback = callback;

        if (this.#isNotNull(this.Callback))
        {
            const params = getParamNames(this.Callback);
            if (params.length >= 2)
            {
                this.ErrorIndex = params.findIndex(n => /^(err|error|e|exception|erro)$/i.test(n));
            }
        }
    }

    UID;
    ClassName;
    Method;
    Data;
    Callback = null;
    ErrorIndex = 1;

    #isNotNull(obj) { return obj !== undefined && obj !== null; }
}


export const WebControllerStatus = 
{
    None         : 'None',
    Identified   : 'Identified',
    Disconnected : 'Disconnected',
    Attempt      : 'Attempt'
}


function getParamNames(fn)
{
    // converte a função em string e pega o conteúdo entre parênteses
    const src = fn.toString();
    const args = src
        .slice(src.indexOf('(') + 1, src.indexOf(')'))
        .split(',')
        .map(a => a.trim())
        .filter(a => a);
    return args; // ex.: ['response','error'] ou ['error','response']
}