/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   Server.hpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:05:17 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/08 22:09:24 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef SERVER_HPP
#define SERVER_HPP

#include "Config.hpp"

class Server{
	private:
		const Config &_cfg;
	public:
		explicit Server(const Config &cfg);
		~Server();
		void run();
};


#endif