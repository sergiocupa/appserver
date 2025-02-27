using System;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;


namespace HttpClientTester
{
    public class TcpClientConnection 
    {

        public void Send(byte[] data)
        {
            try
            {
                if (Stream != null)
                {
                    Stream.Write(data, 0, data.Length);
                    Stream.Flush();
                }
            }
            catch (Exception ex)
            {
                RestoreConnection();
            }
        }


        private void Run()
        {
            Running = true;
            Exited = false;

            while (Running)
            {
                try
                {
                    var res = _Read(Stream);

                    if (res.Continue)
                    {
                        var thr = new Thread((o) => { Received((byte[])o); });
                        thr.Start(res.Data);
                    }
                    else
                    {
                        RestoreConnection();
                        break;
                    }
                }
                catch (Exception ex)
                {
                    RestoreConnection();
                }
            }

            Exited = true;
        }
        private static ResultadoLeitura _Read(NetworkStream clientStream)
        {
            ResultadoLeitura saida = new ResultadoLeitura();
            byte[] message = new byte[4096];
            int bytesRead = 0;

            try
            {
                bytesRead = clientStream.Read(message, 0, 4096);
            }
            catch
            {
                saida.Continue = false;
                return saida;
            }

            if (bytesRead == 0)
            {
                saida.Continue = false;
                return saida;
            }

            byte[] bdata = new byte[bytesRead];
            Array.Copy(message, bdata, bytesRead);
            saida.Data = bdata;

            saida.Continue = true;
            return saida;
        }

        private async void RestoreConnection()
        {
            if (!Restoring)
            {
                Restoring = true;

                tClose = new Thread(() =>
                {
                    _Close();

                    if (MreRestore == null) MreRestore = new ManualResetEvent(false);

                    while (Restoring && !Closing)
                    {
                        try
                        {
                            MreRestore.Reset();
                            _Open();
                            MreRestore.WaitOne(3000);
                            Restoring = false;
                            break;
                        }
                        catch (Exception ex)
                        {
                            Thread.Sleep(100);
                        }
                    }
                });
                tClose.Start();
            }
        }


        public void Close()
        {
            Closing = true;
            _Close();
        }

        public void Open()
        {
            Closing = false;
            Exited  = false;
            _Open();
        }

        private void _Open()
        {
            if (Tcp == null)
            {
                Tcp = new TcpClient(IP, Port);
                Stream = Tcp.GetStream();

                Thr = new Thread(Run);
                Thr.Start();
            }
        }


        private void _Close()
        {
            if (Tcp != null)
            {
                if (MreRestore != null)
                {
                    MreRestore.Set();
                }

                try
                {
                    Running = false;
                }
                catch (Exception ex)
                {
                    string log = "";
                }

                try
                {
                    if(Tcp.Client != null)
                    {
                        Tcp.Client.Shutdown(SocketShutdown.Both);
                    }

                    Tcp.Close();
                }
                catch (Exception ex)
                {
                    string log = "";
                }

                while (!Exited)
                {
                    Thread.Sleep(100);
                }
                Tcp = null;
            }
        }

        private void SetAddress(string address)
        {
            Address = address;
            if (!string.IsNullOrEmpty(address))
            {
                var partes = address.Split(new char[] { ':' }, StringSplitOptions.RemoveEmptyEntries);
                if (partes.Length == 2)
                {
                    string upr = partes[0].ToUpper();
                    if (upr == "LOCALHOST")
                    {
                        IPAddress ipAddress = GetLoopback();
                        IP = ipAddress.ToString();
                    }
                    else IP = partes[0];

                    Port = int.Parse(partes[1]);
                }
            }
        }
        private static IPAddress GetLoopback()
        {
            NetworkInterface[] interfaces = NetworkInterface.GetAllNetworkInterfaces();

            var eth = interfaces.Where(i => (i.NetworkInterfaceType == NetworkInterfaceType.Loopback)).FirstOrDefault();

            IPAddress local = null;
            if (eth != null)
            {
                var ipLocal = eth.GetIPProperties().UnicastAddresses.Where(a => a.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork).FirstOrDefault();
                if (ipLocal != null)
                {
                    local = ipLocal.Address;
                }
            }
            return local;
        }


        Action<byte[]> Received;
        ManualResetEvent MreRestore;
        private bool Restoring;
        private bool Running;
        public Thread tClose;
        private Thread Thr;
        private TcpClient Tcp;
        private NetworkStream Stream;
        private bool Exited;
        private string Address;
        private string IP;
        private int Port;
        private bool Closing;

        public TcpClientConnection(string address, Action<byte[]> received)
        {
            Received = received;

            SetAddress(address);
        }
    }


    internal class ResultadoLeitura
    {
        public bool Continue { get; set; }
        public byte[] Data { get; set; }
    }
}
