/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../StdAfx.h"

#include "mixer.h"

#include "../frame/data_frame.h"

#include "audio/audio_mixer.h"
#include "image/image_mixer.h"

#include "gpu/image/image_mixer.h"
#include "gpu/accelerator.h"
#include "gpu/write_frame.h"

#include <common/env.h>
#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/exception/exceptions.h>
#include <common/gl/gl_check.h>
#include <common/utility/tweener.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <boost/foreach.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm_ext.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/spin_mutex.h>

#include <unordered_map>
#include <vector>

namespace caspar { namespace core {

struct mixed_frame : public data_frame
{
	mutable boost::unique_future<boost::iterator_range<const uint8_t*>>	image_data_;
	const audio_buffer													audio_data_;
	const video_format_desc												video_desc_;
	pixel_format_desc													pixel_desc_;
	const void*															tag_;

public:
	mixed_frame(const void* tag, boost::unique_future<boost::iterator_range<const uint8_t*>>&& image_data, audio_buffer&& audio_data, const video_format_desc& format_desc) 
		: tag_(tag)
		, image_data_(std::move(image_data))
		, audio_data_(std::move(audio_data))
		, video_desc_(format_desc)
		, pixel_desc_(core::pixel_format::bgra)
	{
		pixel_desc_.planes.push_back(core::pixel_format_desc::plane(format_desc.width, format_desc.height, 4));
	}	
	
	const boost::iterator_range<const uint8_t*> image_data(int index = 0) const override
	{
		return image_data_.get();
	}
		
	const boost::iterator_range<uint8_t*> image_data(int) override
	{
		BOOST_THROW_EXCEPTION(invalid_operation());
	}
	
	virtual const struct pixel_format_desc& get_pixel_format_desc() const override
	{
		return pixel_desc_;
	}

	virtual const audio_buffer& audio_data() const override
	{
		return audio_data_;
	}

	virtual audio_buffer& audio_data() override
	{
		BOOST_THROW_EXCEPTION(invalid_operation());
	}

	virtual double get_frame_rate() const override
	{
		return video_desc_.fps;
	}

	virtual int width() const override
	{
		return video_desc_.width;
	}

	virtual int height() const override
	{
		return video_desc_.height;
	}

	virtual const void* tag() const override
	{
		return tag_;
	}
};
		
struct mixer::impl : boost::noncopyable
{			
	safe_ptr<gpu::accelerator>			ogl_;
	
	audio_mixer							audio_mixer_;
	safe_ptr<image_mixer>				image_mixer_;
	
	std::unordered_map<int, blend_mode>	blend_modes_;
			
	executor executor_;

public:
	impl(const safe_ptr<gpu::accelerator>& ogl) 
		: ogl_(ogl)
		, audio_mixer_()
		, image_mixer_(make_safe<gpu::image_mixer>(ogl_))
		, executor_(L"mixer")
	{			
	}	
	
	safe_ptr<const data_frame> operator()(std::map<int, safe_ptr<draw_frame>> frames, const video_format_desc& format_desc)
	{		
		return executor_.invoke([=]() mutable -> safe_ptr<const struct data_frame>
		{		
			try
			{				
				BOOST_FOREACH(auto& frame, frames)
				{
					auto blend_it = blend_modes_.find(frame.first);
					image_mixer_->begin_layer(blend_it != blend_modes_.end() ? blend_it->second : blend_mode::normal);
													
					frame.second->accept(audio_mixer_);					
					frame.second->accept(*image_mixer_);

					image_mixer_->end_layer();
				}

				auto image = (*image_mixer_)(format_desc);
				auto audio = audio_mixer_(format_desc);

				return make_safe<mixed_frame>(this, std::move(image), std::move(audio), format_desc);	
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				return data_frame::empty();
			}	
		});		
	}
					
	void set_blend_mode(int index, blend_mode value)
	{
		executor_.begin_invoke([=]
		{
			auto it = blend_modes_.find(index);
			if(it == blend_modes_.end())
				blend_modes_.insert(std::make_pair(index, value));
			else
				it->second = value;
		}, high_priority);
	}
	
	boost::unique_future<boost::property_tree::wptree> info() const
	{
		boost::promise<boost::property_tree::wptree> info;
		info.set_value(boost::property_tree::wptree());
		return info.get_future();
	}
};
	
mixer::mixer(const safe_ptr<gpu::accelerator>& ogl) 
	: impl_(new impl(ogl)){}
void mixer::set_blend_mode(int index, blend_mode value){impl_->set_blend_mode(index, value);}
boost::unique_future<boost::property_tree::wptree> mixer::info() const{return impl_->info();}
safe_ptr<const data_frame> mixer::operator()(std::map<int, safe_ptr<draw_frame>> frames, const struct video_format_desc& format_desc){return (*impl_)(std::move(frames), format_desc);}
}}